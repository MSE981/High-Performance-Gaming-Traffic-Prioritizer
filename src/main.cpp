#include "App.hpp"
#include "Config.hpp"
#include "Telemetry.hpp"
#include "SelfTest.hpp"
#include <csignal>
#include <print>
#include <sys/eventfd.h>

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
    ::setenv("QT_QPA_EGLFS_HIDECURSOR",   "1",                0); // DSI2 has no DRM cursor plane
    ::setenv("QT_QPA_EGLFS_KMS_CONFIG", "config/kms.json", 0); // disable hwcursor at KMS level: QEglFSKmsGbmIntegration ignores HIDECURSOR

    // 1. Load router and system config
    // Requires the working directory to be the project root (i.e. run as: ./build/app from project root)
    Scalpel::Config::load_config("config/config.txt");

    // Create eventfd pair for iface rescan signalling — must happen before watchdog thread starts
    {
        auto& si = Scalpel::Telemetry::instance().sys_info;
        si.rescan_fd = ::eventfd(0, EFD_CLOEXEC);
        si.done_fd   = ::eventfd(0, EFD_CLOEXEC);
        if (si.rescan_fd < 0 || si.done_fd < 0)
            std::println(stderr, "[Warn] eventfd creation failed — iface refresh button disabled");
    }

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

    // 4. Startup self-test (§3.3.1/3.3.2): runs on a worker thread, main thread joins.
    // Executes before app.start() so data-plane Cores 2/3 are not yet running.
    // Uses independent engine instances — no interference with live routing pipeline.
    {
        Scalpel::SelfTest::SelfTest selftest;

        // §2.2.1: register std::function functor as completion callback
        // §2.2.3: callback receives Report struct (not multi-arg)
        selftest.registerCallback([](const Scalpel::SelfTest::Report& r) {
            Scalpel::SelfTest::LAST_REPORT = r;   // store for GUI notification panel
            std::println("\n╔══ 启动自检结果 ({}/{} 项通过) ══╗", r.passed, r.count);
            for (size_t i = 0; i < r.count; ++i) {
                std::println("  [{}] {}  —  {}",
                    r.cases[i].pass ? "PASS" : "FAIL",
                    r.cases[i].name.data(),
                    r.cases[i].detail.data());
            }
            std::println("╚══════════════════════════════╝");
            if (r.passed < r.count)
                std::println(stderr, "[Warning] {} 项自检失败，请检查硬件或配置",
                    r.count - r.passed);
        });

        selftest.start(); // §3.3.1: spawn worker thread
        selftest.join();  // §3.3.2: block until worker + callback complete
    }

    int ret = 0;
    try {
        if (Scalpel::Config::global_state.enable_gui) {
            // Phase 3: Start Qt GUI mode
            Scalpel::System::Optimizer::set_current_thread_affinity(0); // Core 0: UI/Graphics

            QApplication qapp(argc, argv);
            Scalpel::GUI::Dashboard gui;

            app.start(); // Async start underlying network engine
            gui.showFullScreen();

            // Watchdog thread: when underlying engine receives Ctrl+C (stop_promise set),
            // safely notify Qt to exit GUI. Fixes bug where signal_handler alone couldn't interrupt
            // Qt exec(), causing terminal to hang completely!
            std::thread([&app, &qapp]() {
                Scalpel::System::Optimizer::set_current_thread_affinity(1); // Core 1: Watchdog
                app.wait_for_shutdown();
                QMetaObject::invokeMethod(&qapp, "quit", Qt::QueuedConnection);
            }).detach();

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
    if (Scalpel::Config::SAVE_ON_EXIT.load(std::memory_order_relaxed))
        Scalpel::Config::save_config("config/config.txt");
    std::println("[System] Application cleanly exited. Kernel resources fully released.");

    return ret;
}
