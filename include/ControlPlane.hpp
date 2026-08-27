#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <thread>
#include "DhcpEngine.hpp"
#include "ForwardingState_util.hpp"
#include "NatEngine.hpp"
#include "Scheduler_util.hpp"
#include "Telemetry.hpp"
#include "ParameterServices.hpp"

namespace HPGTP::Control {

// Owns the periodic service threads, the control-event thread and the two
// control-plane eventfds. It drives the ParameterServices tick objects; it does
// not implement their logic.
class ControlPlane {
public:
    ControlPlane(Telemetry& tel, Engine::Nat::NatEngine& nat, Engine::Dhcp::DhcpEngine& dhcp,
                 int lan_fd, Engine::Scheduler::Shaper* dl, Engine::Scheduler::Shaper* ul,
                 double base_dl, double base_ul,
                 std::function<void()> dhcp_applied);
    ~ControlPlane();

    void start(Utils::ForwardState::ForwardingState_util& plane);
    void stop();
    void request_dhcp_config_apply();

private:
    static constexpr size_t WATCHDOG_SERVICE_COUNT = 6;
    std::array<std::thread, WATCHDOG_SERVICE_COUNT> service_threads_{};
    std::thread control_event_thread_;
    std::atomic<bool> running_{false};
    int watchdog_stop_efd_{-1};
    int dhcp_cfg_efd_{-1};

    Telemetry& tel_;
    Engine::Nat::NatEngine& nat_;
    Engine::Dhcp::DhcpEngine& dhcp_;
    int lan_fd_;
    Engine::Scheduler::Shaper* dl_ = nullptr;
    Engine::Scheduler::Shaper* ul_ = nullptr;
    double base_dl_ = 0.0;
    double base_ul_ = 0.0;
    Utils::ForwardState::ForwardingState_util* plane_ = nullptr;
    std::function<void()> dhcp_applied_;

    void wake_watchdog();
    void close_watchdog_stop_efd();
    void close_dhcp_cfg_efd();
    void applyDhcpConfig();
    void watchdog_telemetry_loop();
    void watchdog_l2_refresh_loop();
    void watchdog_wan_tracker_loop();
    void watchdog_dhcp_worker_loop();
    void watchdog_qos_loop();
    void watchdog_nat_ticker_loop();
    void control_event_loop();
};

} // namespace HPGTP::Control
