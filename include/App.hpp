#pragma once
// C++ standard headers only — no POSIX C headers.
// All POSIX C APIs (socket, timerfd, poll, dirent, …) are hidden in App.cpp.
#include <thread>
#include <atomic>
#include <memory>
#include <expected>
#include <future>
#include <array>
#include <span>
#include "NetworkUtils.hpp"
#include "NetworkEngine.hpp"
#include "Processor.hpp"
#include "NatEngine.hpp"
#include "DnsEngine.hpp"
#include "DhcpEngine.hpp"
#include "UpnpEngine.hpp"
#include "SystemOptimizer.hpp"
#include "Telemetry.hpp"
#include "ProbeManager.hpp"
#include "Scheduler.hpp"
#include "FirewallEngine.hpp"

namespace Scalpel {

// ─── Internal data-plane types ───────────────────────────────────────────────
// These are referenced by App's private members and must be layout-complete
// here.  Their *implementations* live in App.cpp together with the POSIX APIs.

// Zero-heap static hash table (FNV-1a) for IP → Shaper mapping.
// Template: must remain in the header.
template<typename T, size_t Capacity = 256>
class StaticIpMap {
public:
    struct Entry {
        Net::IPv4Net key{};
        T            value    = nullptr;
        bool         occupied = false;
    };

private:
    std::array<Entry, Capacity> table{};

    static uint32_t fnv1a_hash(Net::IPv4Net addr) {
        uint32_t val = addr.raw();
        uint32_t h   = 2166136261U;
        h ^= (val & 0xFF);         h *= 16777619U;
        h ^= ((val >>  8) & 0xFF); h *= 16777619U;
        h ^= ((val >> 16) & 0xFF); h *= 16777619U;
        h ^= ((val >> 24) & 0xFF); h *= 16777619U;
        return h;
    }

public:
    template<typename Callback>
    void for_each_occupied(Callback&& cb) {
        for (auto& e : table)
            if (e.occupied && e.value) cb(e.value);
    }

    void insert(Net::IPv4Net ip, T val) {
        uint32_t h = fnv1a_hash(ip) % Capacity;
        for (size_t i = 0; i < Capacity; ++i) {
            size_t idx = (h + i) % Capacity;
            if (!table[idx].occupied || table[idx].key == ip) {
                table[idx] = { ip, val, true };
                return;
            }
        }
    }

    T find(Net::IPv4Net ip) const {
        uint32_t h = fnv1a_hash(ip) % Capacity;
        for (size_t i = 0; i < Capacity; ++i) {
            size_t idx = (h + i) % Capacity;
            if (!table[idx].occupied) return nullptr;
            if (table[idx].key == ip) return table[idx].value;
        }
        return nullptr;
    }
};

// Lock-free double-buffer QoS config (RCU swap pattern).
struct QoSConfig {
    std::array<StaticIpMap<std::shared_ptr<Traffic::Shaper>, 256>, 2> buffers;
    alignas(64) std::atomic<size_t> active_idx{0};

    void update(const std::array<Config::IpLimitEntry, Config::MAX_IP_LIMITS>& table,
                size_t count) {
        size_t active   = active_idx.load(std::memory_order_relaxed);
        size_t inactive = 1 - active;
        buffers[inactive] = {};
        for (size_t i = 0; i < count; ++i)
            buffers[inactive].insert(
                table[i].ip, std::make_shared<Traffic::Shaper>(table[i].rate));
        active_idx.store(inactive, std::memory_order_release);
    }
};

// Configuration bundle passed to each packet worker thread (§2.2.3).
struct PacketWorkerConfig {
    int tx_fd;
    int core_id;
    std::shared_ptr<Traffic::Shaper>       global_shaper;
    std::shared_ptr<Logic::NatEngine>      nat_engine;
    std::shared_ptr<Logic::DnsEngine>      dns_engine;
    std::shared_ptr<QoSConfig>             qos_config;
    std::shared_ptr<QoSConfig>             device_shaper;
    std::shared_ptr<Logic::DhcpEngine>     dhcp_engine;
    std::shared_ptr<Logic::FirewallEngine> firewall_engine;
    Net::IPv4Net gateway_ip{};
};

// ─── Application class ───────────────────────────────────────────────────────
// Public interface: init / start / stop / wait_for_shutdown.
// All POSIX I/O, packet pipeline, and watchdog implementations are in App.cpp.
class App {
    std::unique_ptr<Engine::RawSocketManager> iface_wan;
    std::unique_ptr<Engine::RawSocketManager> iface_lan;
    std::shared_ptr<Traffic::Shaper>          global_shaper;
    std::shared_ptr<Logic::NatEngine>         nat_engine;
    std::shared_ptr<Logic::DnsEngine>         dns_engine;
    std::shared_ptr<Logic::DhcpEngine>        dhcp_engine;
    std::shared_ptr<Logic::FirewallEngine>    firewall_engine;
    std::shared_ptr<Logic::UpnpEngine>        upnp_engine;
    std::shared_ptr<QoSConfig>                qos_config;
    int lan_fd_ = -1;

    std::shared_ptr<Traffic::Shaper> shaper_dl;
    std::shared_ptr<Traffic::Shaper> shaper_ul;
    double base_dl_mbps = 500.0;
    double base_ul_mbps = 50.0;
    std::shared_ptr<QoSConfig> device_shaper_dl;
    std::shared_ptr<QoSConfig> device_shaper_ul;

    std::thread       worker_downstream;
    std::thread       worker_upstream;
    std::thread       watchdog;
    std::atomic<bool> running_workers{false};
    std::atomic<bool> running_watchdog{false};
    std::promise<void> shutdown_promise;
    std::future<void>  shutdown_future;

public:
    App();
    ~App();

    std::expected<void, std::string> init();
    void start();
    void stop() { shutdown_promise.set_value(); }
    void wait_for_shutdown();

private:
    void worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx_mgr,
                           PacketWorkerConfig cfg);
    void watchdog_loop();
};

} // namespace Scalpel
