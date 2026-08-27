#include "ParameterServices.hpp"
#include "Config.hpp"
#include "Types_util.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>

namespace HPGTP::ParameterServices {

void TelemetryCollector::readSysfd(const char* path, std::span<char> out) {
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) { out[0] = '\0'; return; }
    const ssize_t n = ::read(fd, out.data(), out.size() - 1);
    ::close(fd);
    if (n > 0) {
        out[static_cast<size_t>(n)] = '\0';
        if (out[n - 1] == '\n') out[n - 1] = '\0';
    } else {
        out[0] = '\0';
    }
}

void TelemetryCollector::refreshCpu() {
    std::array<char, 1024> sbuf{};
    readSysfd("/proc/stat", sbuf);
    if (sbuf[0] == '\0') return;
    const char* p = sbuf.data();
    long nproc = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1) nproc = 1;
    const int max_ci = static_cast<int>(
        std::min<long>(nproc, static_cast<long>(tel_.core_metrics.size())));
    for (int ci = 0; ci < max_ci; ++ci) {
        char tag[8]{};
        std::snprintf(tag, sizeof(tag), "cpu%d ", ci);
        const char* ln = std::strstr(p, tag);
        if (ln == nullptr) break;
        ln += std::strlen(tag);
        uint64_t user{0}, nice{0}, sys{0}, idle{0}, iowait{0}, irq{0}, softirq{0};
        if (std::sscanf(ln, "%lu %lu %lu %lu %lu %lu %lu",
                &user, &nice, &sys, &idle, &iowait, &irq, &softirq) == 7) {
            const uint64_t total = user + nice + sys + idle + iowait + irq + softirq;
            const uint64_t dt = total - stat_total_[ci];
            const uint64_t di = idle - stat_idle_[ci];
            const int pct = (dt > 0) ? static_cast<int>(100 * (dt - di) / dt) : 0;
            tel_.core_metrics[ci].cpu_load_pct.store(pct, std::memory_order_relaxed);
            stat_total_[ci] = total;
            stat_idle_[ci]  = idle;
        }
    }
}

void TelemetryCollector::refreshDevices() {
    std::array<char, 4096> arp{};
    const int afd = ::open("/proc/net/arp", O_RDONLY);
    if (afd < 0) return;
    const ssize_t nr = ::read(afd, arp.data(), arp.size() - 1);
    ::close(afd);
    if (nr <= 0) return;

    char* line = arp.data();
    char* end  = arp.data() + nr;
    while (line < end && *line != '\n') ++line;
    if (line < end) ++line;
    uint8_t dcnt = 0;
    bool changed = false;
    const uint8_t prev_cnt = tel_.device_count.load(std::memory_order_relaxed);
    while (line < end && dcnt < Telemetry::MAX_TRACKED_DEVICES) {
        char ip_str[20]{}, hw[8]{}, flags[8]{}, mac[20]{};
        if (std::sscanf(line, "%19s %7s %7s %19s", ip_str, hw, flags, mac) == 4) {
            Net::IPv4Net ip{};
            if (Net::try_parse_ipv4(ip_str, ip) && std::strcmp(flags, "0x0") != 0) {
                auto& slot = tel_.device_table[dcnt];
                if (slot.ip != ip) { slot.ip = ip; changed = true; }
                std::array<char, 18> mac_buf{};
                const std::string_view mac_sv{mac};
                const auto nm = mac_sv.copy(mac_buf.data(), 17);
                mac_buf[nm] = '\0';
                if (std::memcmp(slot.mac.data(), mac_buf.data(), mac_buf.size()) != 0) {
                    slot.mac = mac_buf;
                    changed  = true;
                }
                ++dcnt;
            }
        }
        while (line < end && *line != '\n') ++line;
        if (line < end) ++line;
    }
    if (dcnt != prev_cnt) changed = true;
    tel_.device_count.store(dcnt, std::memory_order_release);
    if (changed)
        tel_.device_table_revision.fetch_add(1, std::memory_order_release);
}

void TelemetryCollector::tick1Hz() {
    char tbuf[16]{};
    readSysfd("/sys/class/thermal/thermal_zone0/temp", tbuf);
    if (tbuf[0] != '\0')
        tel_.cpu_temp_celsius.store(std::atof(tbuf) / 1000.0, std::memory_order_relaxed);
    refreshCpu();
}

void TelemetryCollector::tick5s() {
    auto& si = tel_.sys_info;
    readSysfd("/etc/hostname", std::span<char>(si.hostname));
    readSysfd("/proc/version", std::span<char>(si.kernel_short));

    char ubuf[32]{};
    readSysfd("/proc/uptime", ubuf);
    if (ubuf[0] != '\0')
        si.uptime_seconds.store(static_cast<uint64_t>(std::atof(ubuf)), std::memory_order_relaxed);

    char mbuf[512]{};
    readSysfd("/proc/meminfo", mbuf);
    if (mbuf[0] != '\0') {
        uint64_t total{0}, avail{0};
        const char* mt = std::strstr(mbuf, "MemTotal:");
        const char* ma = std::strstr(mbuf, "MemAvailable:");
        if (mt != nullptr) (void)std::sscanf(mt, "MemTotal: %lu", &total);
        if (ma != nullptr) (void)std::sscanf(ma, "MemAvailable: %lu", &avail);
        si.mem_total_kb.store(total, std::memory_order_relaxed);
        si.mem_avail_kb.store(avail, std::memory_order_relaxed);
    }
    refreshDevices();
}

void NatWanTracker::tick5s() {
    if (!Config::global_state.enable_nat.load(std::memory_order_relaxed))
        return;
    const std::string s = source_();
    if (s.empty()) return;
    const auto e = Config::parse_ip_str(s);
    if (!e) return;
    const uint32_t h = ntohl(e->raw());
    if (h == 0 || (h >> 24) == 127) return;
    if (e->raw() == nat_.wan_ip_snapshot().raw()) return;
    nat_.set_wan_ip(*e);
}

void QosController::tick1Hz() {
    if (tel_.mode_config_dirty.exchange(false, std::memory_order_acq_rel)) {
        const bool accel_on = tel_.acceleration_pending.load(std::memory_order_relaxed);
        Config::ENABLE_ACCELERATION.store(accel_on, std::memory_order_relaxed);
        tel_.effective_acceleration.store(accel_on, std::memory_order_release);
        tel_.effective_bridge_mode.store(!accel_on, std::memory_order_release);
    }
    if (tel_.qos_global_bw_dirty.exchange(false, std::memory_order_acq_rel)) {
        base_dl_ = tel_.qos_global_dl_mbps_pending.load(std::memory_order_relaxed);
        base_ul_ = tel_.qos_global_ul_mbps_pending.load(std::memory_order_relaxed);
        tel_.effective_qos_global_dl_mbps.store(base_dl_, std::memory_order_release);
        tel_.effective_qos_global_ul_mbps.store(base_ul_, std::memory_order_release);
        if (shaper_dl_ != nullptr)
            shaper_dl_->set_rate_limit(Traffic::Mbps{base_dl_});
        if (shaper_ul_ != nullptr)
            shaper_ul_->set_rate_limit(Traffic::Mbps{base_ul_});
    }
}

} // namespace HPGTP::ParameterServices
