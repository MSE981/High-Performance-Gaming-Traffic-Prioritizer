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
#include "NetworkEngine.hpp"
#include "Processor.hpp"
#include "NatEngine.hpp"
#include "DhcpEngine.hpp"
#include "SystemOptimizer_util.hpp"
#include "Telemetry.hpp"
#include "Scheduler_util.hpp"
#include "EventCallbacks_util.hpp"
#include "ForwardingState_util.hpp"
#include "Forward_Engine.hpp"
#include "ControlPlane.hpp"
#include "NetworkConfig.hpp"
#include "Lifecycle.hpp"

// HPGTP: High-Performance Gaming Traffic Prioritizer. Root namespace for all
// product code (nested: Logic, Net, GUI, Traffic, Engine, ...).
namespace HPGTP {

// Internal data-plane types
// These are referenced by App's private members and must be layout-complete
// here. Their *implementations* live in App.cpp together with the POSIX APIs.

// Application class
// Public interface: init / start / stop / wait_for_shutdown.
// All POSIX I/O, packet pipeline, and watchdog implementations are in App.cpp.
class App {
    ForwardState_util::ForwardingState_util       forwarding_plane_;
    std::unique_ptr<Engine::RawSocketManager> iface_wan;
    std::unique_ptr<Engine::RawSocketManager> iface_lan;
    std::unique_ptr<Logic::NatEngine>         nat_engine_;
    std::unique_ptr<Logic::DhcpEngine>        dhcp_engine_;
    int lan_fd_ = -1;

    Events_util::CallbackRegistry         dl_events_;
    Events_util::CallbackRegistry         ul_events_;
    std::unique_ptr<Traffic::Shaper> shaper_dl_;
    std::unique_ptr<Traffic::Shaper> shaper_ul_;
    double base_dl_mbps = 500.0;
    double base_ul_mbps = 50.0;

    Lifecycle::Lifecycle lifecycle_;
    std::unique_ptr<Events_util::PacketObserver> telemetry_observer_;
    std::unique_ptr<ForwardEngine::Forward_Engine> forward_engine_;
    std::unique_ptr<Control::ControlPlane> control_plane_;
    std::unique_ptr<NetConfig::NetworkConfig> netcfg_;

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
};

} // namespace HPGTP
