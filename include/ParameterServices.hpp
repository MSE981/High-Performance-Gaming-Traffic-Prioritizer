#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include "Telemetry.hpp"
#include "NatEngine.hpp"
#include "DhcpEngine.hpp"
#include "Scheduler_util.hpp"

namespace HPGTP::ParameterServices {

// 1 s: CPU temperature and per-core load. 5 s: hostname/kernel/uptime/memory and the ARP device table. Everything goes into Telemetry for the GUI.
class TelemetryCollector {
public:
    explicit TelemetryCollector(Telemetry& tel) : tel_(tel) {}
    void tick1Hz();
    void tick5s();

private:
    void readSysfd(const char* path, std::span<char> out);
    void refreshCpu();
    void refreshDevices();

    Telemetry& tel_;
    std::array<uint64_t, 4> stat_total_{};
    std::array<uint64_t, 4> stat_idle_{};
};

// 5 s: refresh the L2 forwarding snapshot (WAN/LAN MACs, gateway ARP, prefix).
class L2ForwardRefresher {
public:
    explicit L2ForwardRefresher(std::function<void()> refresh) : refresh_(std::move(refresh)) {}
    void tick5s() { if (refresh_) refresh_(); }

private:
    std::function<void()> refresh_;
};

// 5 s: follow the kernel-assigned WAN address (NetworkManager owns it).
class NatWanTracker {
public:
    NatWanTracker(Logic::NatEngine& nat, std::function<std::string()> source)
        : nat_(nat), source_(std::move(source)) {}
    void tick5s();

private:
    Logic::NatEngine& nat_;
    std::function<std::string()> source_;
};

// DHCP background work (response queue / lease upkeep) plus router IP sync.
class DhcpWorker {
public:
    DhcpWorker(Logic::DhcpEngine& dhcp, int& lan_fd, std::function<void()> refresh_router)
        : dhcp_(dhcp), lan_fd_(lan_fd), refresh_router_(std::move(refresh_router)) {}
    void tick1Hz() { dhcp_.process_background_tasks(lan_fd_); }
    void tick5s() { if (refresh_router_) refresh_router_(); }

private:
    Logic::DhcpEngine& dhcp_;
    int& lan_fd_;
    std::function<void()> refresh_router_;
};

// Applies GUI mode (Acceleration/Bridge) and global DL/UL bandwidth caps. Throttle percentage was removed; caps are applied as-is.
class QosController {
public:
    QosController(Telemetry& tel, Traffic::Shaper* dl, Traffic::Shaper* ul,
                  double& base_dl, double& base_ul)
        : tel_(tel), shaper_dl_(dl), shaper_ul_(ul),
          base_dl_(base_dl), base_ul_(base_ul) {}
    void tick1Hz();

private:
    Telemetry& tel_;
    Traffic::Shaper* shaper_dl_;
    Traffic::Shaper* shaper_ul_;
    double& base_dl_;
    double& base_ul_;
};

// 1 Hz: NAT session expiry cleanup.
class EngineTicker {
public:
    explicit EngineTicker(Logic::NatEngine& nat) : nat_(nat) {}
    void tick1Hz() { nat_.tick(); }

private:
    Logic::NatEngine& nat_;
};

} // namespace HPGTP::ParameterServices
