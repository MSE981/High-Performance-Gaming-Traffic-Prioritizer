#include "App.hpp"
#include "Config.hpp"
#include "Telemetry.hpp"
#include "SelfTest.hpp"
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
    ::setenv("QT_QPA_EGLFS_HIDECURSOR",   "1",                0); // DSI2 has no DRM cursor plane
    ::setenv("QT_QPA_EGLFS_KMS_CONFIG", "config/kms.json", 0); // disable hwcursor at KMS level: QEglFSKmsGbmIntegration ignores HIDECURSOR

    // 1. Load router and system config
    // Requires the working directory to be the project root (i.e. run as: ./build/app from project root)
    Scalpel::Config::load_config("config/config.txt");

    // Create eventfd pair for iface rescan signalling — must happen before watchdog thread starts
    Scalpel::Telemetry::instance().sys_info.init_event_fds();

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
            // Core 0: UI/Graphics — must be set before QApplication construction
            Scalpel::System::Optimizer::set_current_thread_affinity(0);

            QApplication qapp(argc, argv);
            Scalpel::GUI::Dashboard gui;
            gui.showFullScreen();

            // Async self-test: GUI shows immediately with overlay; callback fires on Qt main
            // thread after test completes so app.start() is called only once path is clear.
            // Prevents a burst of queued packets from flooding the engine on first tick.
            // selftest outlives qapp.exec() so the destructor join() is a no-op (thread done).
            Scalpel::SelfTest::SelfTest selftest;
            selftest.registerCallback([&app](const Scalpel::SelfTest::Report& r) {
                Scalpel::SelfTest::LAST_REPORT = r;
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

                // Post to Qt main thread (Core 0) — never call Qt from worker thread directly
                QMetaObject::invokeMethod(QCoreApplication::instance(), [&app, r]() {
                    app.start();
                    Scalpel::GUI::Dashboard::on_selftest_done(r);
                }, Qt::QueuedConnection);
            });
            selftest.start(); // worker thread launched; event loop handles callback

            // Watchdog thread: when underlying engine receives Ctrl+C (stop set),
            // safely notify Qt to exit GUI. Fixes bug where signal_handler alone couldn't
            // interrupt Qt exec(), causing terminal to hang completely.
            std::thread([&app, &qapp]() {
                Scalpel::System::Optimizer::set_current_thread_affinity(1); // Core 1: Watchdog
                app.wait_for_shutdown();
                QMetaObject::invokeMethod(&qapp, "quit", Qt::QueuedConnection);
            }).detach();

            ret = qapp.exec(); // Block on main event loop

            // If user closed the window (no UNIX signal), notify underlying engine to stop
            if (!signal_received.exchange(true)) {
                app.stop();
            }
        } else {
            // Pure CLI mode — run selftest synchronously then start engine
            {
                Scalpel::SelfTest::SelfTest selftest;
                selftest.registerCallback([](const Scalpel::SelfTest::Report& r) {
                    Scalpel::SelfTest::LAST_REPORT = r;
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
                selftest.start();
                selftest.join();
            }
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
