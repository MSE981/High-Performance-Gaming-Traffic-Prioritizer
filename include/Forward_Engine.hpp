#pragma once
#include <array>
#include <atomic>
#include <expected>
#include <memory>
#include <string>
#include <thread>
#include "DhcpEngine.hpp"
#include "EventCallbacks_util.hpp"
#include "ForwardingState_util.hpp"
#include "NatEngine.hpp"
#include "NetworkEngine.hpp"
#include "Scheduler_util.hpp"

namespace HPGTP {

// Per-thread routing and engine handles passed into each packet worker.
struct PacketWorkerConfig {
    int tx_fd{};
    int core_id{};
    Traffic::Shaper*            route_shaper = nullptr;
    Logic::NatEngine*           nat_engine = nullptr;
    Logic::DhcpEngine*          dhcp_engine = nullptr;
    Events_util::CallbackRegistry* callbacks = nullptr;
};

} // namespace HPGTP

namespace HPGTP::ForwardEngine {

// Owns the two data-plane workers (WAN->LAN on core 2, LAN->WAN on core 3)
// and their per-worker eventfds. App remains the owner of the engines and
// shapers; this class borrows them through raw pointers.
class Forward_Engine {
public:
    Forward_Engine() = default;
    ~Forward_Engine();

    std::expected<void, std::string> start(
        std::unique_ptr<Engine::RawSocketManager> wan,
        std::unique_ptr<Engine::RawSocketManager> lan,
        int fd_wan, int fd_lan,
        Traffic::Shaper* shaper_dl, Traffic::Shaper* shaper_ul,
        Logic::NatEngine* nat_engine, Logic::DhcpEngine* dhcp_engine,
        Events_util::CallbackRegistry* dl_events,
        Events_util::CallbackRegistry* ul_events,
        ForwardState_util::ForwardingState_util& plane);
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

    std::unique_ptr<Engine::RawSocketManager> wan_;
    std::unique_ptr<Engine::RawSocketManager> lan_;
    int fd_wan_ = -1;
    int fd_lan_ = -1;
    Traffic::Shaper* shaper_dl_ = nullptr;
    Traffic::Shaper* shaper_ul_ = nullptr;
    Logic::NatEngine* nat_engine_ = nullptr;
    Logic::DhcpEngine* dhcp_engine_ = nullptr;
    Events_util::CallbackRegistry* dl_events_ = nullptr;
    Events_util::CallbackRegistry* ul_events_ = nullptr;
    ForwardState_util::ForwardingState_util* plane_ = nullptr;

    std::expected<void, std::string> open_poll_fds();
    void close_poll_fds();
    void wake_workers();
    void worker_event_loop(std::unique_ptr<Engine::RawSocketManager> rx_mgr,
                           PacketWorkerConfig cfg,
                           WorkerPollSync& poll_sync);
};

} // namespace HPGTP::ForwardEngine
