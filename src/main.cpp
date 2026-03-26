#include "App.hpp"
#include "Config.hpp"
#include <csignal>
#include <print>

// Global pointer and signal debounce flag
Scalpel::App* global_app = nullptr;
std::atomic<bool> signal_received{false};

// Signal handler entry point
extern "C" void signal_handler(int signal) {
    (void)signal;
    if (signal_received.exchange(true)) return; // Intercept double shutdown

    // Note: don't execute heavy logic directly in async UNIX signal, just trigger shutdown
    if (global_app) {
        global_app->stop();
    }
}

#include <QApplication>
#include <QMetaObject>
#include <thread>
#include "GUI/Dashboard.hpp"
#include "SystemOptimizer.hpp"

int main(int argc, char* argv[]) {
    // Ignore SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);

    // 1. Load router and system config
    Scalpel::Config::load_config();

    Scalpel::App app;
    global_app = &app;

    // 2. Register shutdown signals
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 3. Low-level system initialization
    if (auto res = app.init(); !res) {
        std::println(stderr, "[Fatal Error] Initialization failed: {}", res.error());
        return 1;
    }

    int ret = 0;
    try {
        if (Scalpel::Config::global_state.enable_gui) {
            // Phase 3: Start Qt GUI mode
            Scalpel::System::Optimizer::set_current_thread_affinity(0); // Core 0: UI/Graphics
            QApplication qapp(argc, argv);
            Scalpel::GUI::Dashboard gui;

            app.start(); // Async start underlying network engine
            gui.show();

            // Watchdog thread: when underlying engine receives Ctrl+C (stop_promise set),
            // safely notify Qt to exit GUI. Fixes bug where signal_handler alone couldn't interrupt
            // Qt exec(), causing terminal to hang completely!
            std::jthread qt_stopper([&app, &qapp]() {
                Scalpel::System::Optimizer::set_current_thread_affinity(1); // Core 1: Watchdog
                app.wait_for_shutdown();
                QMetaObject::invokeMethod(&qapp, "quit", Qt::QueuedConnection);
            });

            ret = qapp.exec(); // Block on main event loop

            // If user clicked window X button to close GUI, no UNIX signal generated,
            // must notify underlying engine to stop
            if (!signal_received.exchange(true)) {
                app.stop();
            }
        } else {
            // Pure CLI mode, block waiting for exit
            app.start();
            app.wait_for_shutdown();
        }
    } catch (const std::exception& e) {
        std::println(stderr, "[Fatal Error] Uncaught exception: {}", e.what());
        return 1;
    }

    global_app = nullptr;
    Scalpel::Config::save_config();
    std::println("[System] Application cleanly exited. Kernel resources fully released.");

    return ret;
}
