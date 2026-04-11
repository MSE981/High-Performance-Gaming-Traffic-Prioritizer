#include "App.hpp"
#include "Config.hpp"
#include "Telemetry.hpp"
#include "SelfTest.hpp"
#include <csignal>
#include <cerrno>
#include <cstring>
#include <print>
#include <thread>
#include <atomic>
#include <sys/eventfd.h>
#include <unistd.h>

// Global pointer and signal debounce flag
Scalpel::App* global_app = nullptr;
std::atomic<bool> signal_received{false};

// Written only from signal_handler (async-signal-safe path); read on a normal thread.
static int shutdown_signal_efd = -1;

// Async-signal-safe: only set flag and bump eventfd; never join threads here.
extern "C" void signal_handler(int signal) {
    (void)signal;
    if (signal_received.exchange(true, std::memory_order_acq_rel)) return;
    if (shutdown_signal_efd >= 0)
        (void)::eventfd_write(shutdown_signal_efd, 1);
}

#include <QApplication>
#include <QMetaObject>
#include <QSocketNotifier>
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

    if (auto r = Scalpel::Telemetry::instance().sys_info.init_event_fds(); !r) {
        std::println(stderr, "[Fatal] {}", r.error());
        return 1;
    }

    shutdown_signal_efd = ::eventfd(0, EFD_CLOEXEC);
    if (shutdown_signal_efd < 0) {
        std::println(stderr, "[Fatal] shutdown eventfd: {}", std::strerror(errno));
        return 1;
    }

    Scalpel::App app;
    global_app = &app;

    // 2. Register shutdown signals (handler must not call App::stop)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 3. Low-level system initialization
    if (auto res = app.init(); !res) {
        std::println(stderr, "[Fatal Error] Initialization failed: {}", res.error());
        ::close(shutdown_signal_efd);
        shutdown_signal_efd = -1;
        global_app = nullptr;
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

            QSocketNotifier shutdown_sn(shutdown_signal_efd, QSocketNotifier::Read, &qapp);
            QObject::connect(&shutdown_sn, &QSocketNotifier::activated, &qapp, [](int) {
                uint64_t v;
                (void)::eventfd_read(shutdown_signal_efd, &v);
                if (global_app) global_app->stop();
            });

            // Async self-test: GUI shows with an overlay; when the test finishes the callback
            // runs on the Qt main thread and calls app.start() once. selftest is destroyed after
            // qapp.exec() returns, so its worker thread is already joined.
            Scalpel::SelfTest::SelfTest selftest;
            selftest.registerCallback([&app](const Scalpel::SelfTest::Report& r) {
                Scalpel::SelfTest::LAST_REPORT = r;
                std::println("\n=== Startup self-test: {} / {} passed ===", r.passed, r.count);
                for (size_t i = 0; i < r.count; ++i) {
                    std::println("  [{}] {} : {}",
                        r.cases[i].pass ? "PASS" : "FAIL",
                        r.cases[i].name.data(),
                        r.cases[i].detail.data());
                }
                std::println("=== End self-test ===");
                if (r.passed < r.count)
                    std::println(stderr,
                        "[Warning] {} self-test case(s) failed; review hardware and configuration.",
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
            std::thread watchdog_notify([&app, &qapp]() {
                Scalpel::System::Optimizer::set_current_thread_affinity(1); // Core 1: Watchdog
                app.wait_for_shutdown();
                QMetaObject::invokeMethod(&qapp, "quit", Qt::QueuedConnection);
            });

            ret = qapp.exec(); // Block on main event loop

            // If user closed the window (no UNIX signal), notify underlying engine to stop
            if (!signal_received.exchange(true, std::memory_order_acq_rel)) {
                app.stop();
            }
            watchdog_notify.join(); // qapp still in scope; thread is done on both exit paths
        } else {
            // Pure CLI mode: dedicated thread reads shutdown eventfd and calls App::stop().
            std::atomic<bool> cli_listener_run{true};
            std::thread cli_shutdown_listener([&cli_listener_run] {
                while (cli_listener_run.load(std::memory_order_relaxed)) {
                    uint64_t v;
                    int      r = ::eventfd_read(shutdown_signal_efd, &v);
                    if (r < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (!cli_listener_run.load(std::memory_order_relaxed)) break;
                    if (global_app) global_app->stop();
                }
            });

            {
                Scalpel::SelfTest::SelfTest selftest;
                selftest.registerCallback([](const Scalpel::SelfTest::Report& r) {
                    Scalpel::SelfTest::LAST_REPORT = r;
                    std::println("\n=== Startup self-test: {} / {} passed ===", r.passed, r.count);
                    for (size_t i = 0; i < r.count; ++i) {
                        std::println("  [{}] {} : {}",
                            r.cases[i].pass ? "PASS" : "FAIL",
                            r.cases[i].name.data(),
                            r.cases[i].detail.data());
                    }
                    std::println("=== End self-test ===");
                    if (r.passed < r.count)
                        std::println(stderr,
                            "[Warning] {} self-test case(s) failed; review hardware and configuration.",
                            r.count - r.passed);
                });
                selftest.start();
                selftest.join();
            }
            app.start();
            app.wait_for_shutdown();
            cli_listener_run.store(false, std::memory_order_relaxed);
            (void)::eventfd_write(shutdown_signal_efd, 1);
            cli_shutdown_listener.join();
        }
    } catch (const std::exception& e) {
        std::println(stderr, "[Fatal Error] Uncaught exception: {}", e.what());
        if (shutdown_signal_efd >= 0) {
            ::close(shutdown_signal_efd);
            shutdown_signal_efd = -1;
        }
        global_app = nullptr;
        return 1;
    }

    global_app = nullptr;
    if (shutdown_signal_efd >= 0) {
        ::close(shutdown_signal_efd);
        shutdown_signal_efd = -1;
    }
    if (Scalpel::Config::SAVE_ON_EXIT.load(std::memory_order_relaxed))
        Scalpel::Config::save_config("config/config.txt");
    std::println("[System] Application cleanly exited. Kernel resources fully released.");

    return ret;
}
