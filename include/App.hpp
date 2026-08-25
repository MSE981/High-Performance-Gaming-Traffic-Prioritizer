#pragma once
// C++ standard headers only -- no POSIX C headers.
// All POSIX C APIs (socket, poll, dirent, ...) are hidden in App.cpp.
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <string>
#include <expected>
#include <condition_variable>
#include <array>
#include <span>
#include "NetworkUtils.hpp"
#include "NetworkEngine.hpp"
#include "Processor.hpp"
#include "NatEngine.hpp"
#include "DhcpEngine.hpp"
#include "SystemOptimizer.hpp"
#include "Telemetry.hpp"
#include "Scheduler.hpp"
#include "EventCallbacks.hpp"
#include "ForwardingPlane.hpp"

// HPGTP: High-Performance Gaming Traffic Prioritizer. Root namespace for all
// product code (nested: Logic, Net, GUI, Traffic, Engine, ...).
namespace HPGTP {

// Internal data-plane types
// These are referenced by App's private members and must be layout-complete
// here. Their *implementations* live in App.cpp together with the POSIX APIs.

// Per-thread routing and engine handles passed into each packet worker.
struct PacketWorkerConfig {
    int tx_fd{};
    int core_id{};
    Traffic::Shaper*            route_shaper = nullptr;
    Logic::NatEngine*           nat_engine = nullptr;
    Logic::DhcpEngine*          dhcp_engine = nullptr;
    Events::CallbackRegistry*   callbacks = nullptr;
};

// Application class
// Public interface: init / start / stop / wait_for_shutdown.
// All POSIX I/O, packet pipeline, and watchdog implementations are in App.cpp.
class App {
    DataPlane::ForwardingPlane       forwarding_plane_;
    std::unique_ptr<Engine::RawSocketManager> iface_wan;
    std::unique_ptr<Engine::RawSocketManager> iface_lan;
    std::unique_ptr<Logic::NatEngine>         nat_engine_;
    std::unique_ptr<Logic::DhcpEngine>        dhcp_engine_;
    int lan_fd_ = -1;

    Events::CallbackRegistry         dl_events_;
    Events::CallbackRegistry         ul_events_;
    std::unique_ptr<Traffic::Shaper> shaper_dl_;
    std::unique_ptr<Traffic::Shaper> shaper_ul_;
    double base_dl_mbps = 500.0;
    double base_ul_mbps = 50.0;

    std::thread       worker_downstream;
    std::thread       worker_upstream;
    static constexpr size_t WATCHDOG_SERVICE_COUNT = 6;
    std::array<std::thread, WATCHDOG_SERVICE_COUNT> service_threads{};
    std::thread       control_event_thread;
    std::atomic<bool> running_workers{false};
    std::atomic<bool> running_watchdog{false};
    std::atomic<bool>   shutdown_complete_{false};
    std::mutex          shutdown_mutex_;
    std::condition_variable shutdown_cv_;
    std::atomic<bool>   shutdown_sequence_started_{false};
    std::unique_ptr<Events::PacketObserver> telemetry_observer_;

    struct WorkerPollSync {
        int frame_efd{-1};
        int stop_efd{-1};
    };
    std::array<WorkerPollSync, 2> worker_poll_{};
    int watchdog_stop_efd_{-1};
    int dhcp_cfg_efd_{-1};

    std::expected<void, std::string> open_worker_poll_fds_for_start();
    void close_worker_poll_fds();
    void wake_proc_threads_for_shutdown();
    void wake_watchdog_for_shutdown();
    void close_watchdog_stop_efd();
    void close_dhcp_cfg_efd();

    // DHCP pool subnet alignment + kernel LAN IP as DhcpEngine router_ip (Core 1 / init).
    Net::IPv4Net                     effective_lan_gateway_{};
    std::expected<void, std::string> sync_lan_subnet_and_dhcp_gateway();
    void                             refresh_dhcp_router_from_kernel() noexcept;

public:
    App();
    ~App();

    std::expected<void, std::string> init();
    void start();
    void stop();
    void wait_for_shutdown();
    // GUI writes this after changing DHCP fields: it signals the control thread.
    void request_dhcp_config_apply();

private:
    void worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx_mgr,
                           PacketWorkerConfig cfg,
                           WorkerPollSync& poll_sync);
    void watchdog_telemetry_loop();
    void watchdog_l2_refresh_loop();
    void watchdog_wan_tracker_loop();
    void watchdog_dhcp_worker_loop();
    void watchdog_qos_loop();
    void watchdog_nat_ticker_loop();
    void control_event_loop();
    void applyDhcpConfig();
};

} // namespace HPGTP
