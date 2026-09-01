#pragma once
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <string>
#include <expected>
#include <condition_variable>
#include <array>
#include <span>
#include "Network_util.hpp"
#include "RawSocket_util.hpp"
#include "Processor.hpp"
#include "NatEngine.hpp"
#include "DhcpEngine.hpp"
#include "SystemOptimizer_util.hpp"
#include "Telemetry.hpp"
#include "Scheduler_util.hpp"
#include "EventCallbacks_util.hpp"
#include "ForwardingState_util.hpp"
#include "ControlPlane.hpp"
#include "NetworkConfig.hpp"
#include "Lifecycle.hpp"

// HPGTP: High-Performance Gaming Traffic Prioritizer. Root namespace for all product code
namespace HPGTP {

namespace Engine::Forward {

// Per-thread routing and engine handles passed into each packet worker.
struct PacketWorkerConfig {
    int tx_fd{};
    int core_id{};
    Engine::Scheduler::Shaper*      route_shaper = nullptr;
    Engine::Nat::NatEngine*         nat_engine = nullptr;
    Engine::Dhcp::DhcpEngine*       dhcp_engine = nullptr;
    Utils::Events::CallbackRegistry* callbacks = nullptr;
};

// Owns the two data-plane workers and their per-worker eventfds. App remains the owner of the engines and shapers; this class borrows them through raw pointers.
class Forward_Engine {
public:
    Forward_Engine() = default;
    ~Forward_Engine();

    std::expected<void, std::string> start(
        std::unique_ptr<Utils::RawSocket::RawSocketManager> wan,
        std::unique_ptr<Utils::RawSocket::RawSocketManager> lan,
        int fd_wan, int fd_lan,
        Engine::Scheduler::Shaper* shaper_dl, Engine::Scheduler::Shaper* shaper_ul,
        Engine::Nat::NatEngine* nat_engine, Engine::Dhcp::DhcpEngine* dhcp_engine,
        Utils::Events::CallbackRegistry* dl_events,
        Utils::Events::CallbackRegistry* ul_events,
        Utils::ForwardState::ForwardingState_util& plane);
    void stop();

private:
    struct WorkerPollSync {
        int frame_efd{-1};
        int stop_efd{-1};
    };

    std::array<WorkerPollSync, 2> poll_sync_{};
    std::thread worker_downstream_;
    std::thread worker_upstream_;
    std::atomic<bool> running_{false};

    std::unique_ptr<Utils::RawSocket::RawSocketManager> wan_;
    std::unique_ptr<Utils::RawSocket::RawSocketManager> lan_;
    int fd_wan_ = -1;
    int fd_lan_ = -1;
    Engine::Scheduler::Shaper* shaper_dl_ = nullptr;
    Engine::Scheduler::Shaper* shaper_ul_ = nullptr;
    Engine::Nat::NatEngine* nat_engine_ = nullptr;
    Engine::Dhcp::DhcpEngine* dhcp_engine_ = nullptr;
    Utils::Events::CallbackRegistry* dl_events_ = nullptr;
    Utils::Events::CallbackRegistry* ul_events_ = nullptr;
    Utils::ForwardState::ForwardingState_util* plane_ = nullptr;

    std::expected<void, std::string> open_poll_fds();
    void close_poll_fds();
    void wake_workers();
    void worker_event_loop(std::unique_ptr<Utils::RawSocket::RawSocketManager> rx_mgr,
                           PacketWorkerConfig cfg,
                           WorkerPollSync& poll_sync);
};

} // namespace Engine::Forward


// Application class
// Public interface: init / start / stop / wait_for_shutdown.
class App {
    Utils::ForwardState::ForwardingState_util       forwarding_plane_;
    std::unique_ptr<Utils::RawSocket::RawSocketManager> iface_wan;
    std::unique_ptr<Utils::RawSocket::RawSocketManager> iface_lan;
    std::unique_ptr<Engine::Nat::NatEngine>         nat_engine_;
    std::unique_ptr<Engine::Dhcp::DhcpEngine>        dhcp_engine_;
    int lan_fd_ = -1;

    Utils::Events::CallbackRegistry         dl_events_;
    Utils::Events::CallbackRegistry         ul_events_;
    std::unique_ptr<Engine::Scheduler::Shaper> shaper_dl_;
    std::unique_ptr<Engine::Scheduler::Shaper> shaper_ul_;
    double base_dl_mbps = 500.0;
    double base_ul_mbps = 50.0;

    Lifecycle::Lifecycle lifecycle_;
    std::unique_ptr<Utils::Events::PacketObserver> telemetry_observer_;
    std::unique_ptr<Engine::Forward::Forward_Engine> forward_engine_;
    std::unique_ptr<Control::ControlPlane> control_plane_;
    std::unique_ptr<NetConfig::NetworkConfig> netcfg_;

public:
    App();
    ~App();

    std::expected<void, std::string> init();
    void start();
    void stop();
    void wait_for_shutdown();
    void request_dhcp_config_apply();

private:
};

} // namespace HPGTP
