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

std::atomic<bool> signal_received{false};

static std::atomic<int> shutdown_signal_efd{-1};

extern "C" void signal_handler(int signal) {
    (void)signal;
    if (signal_received.exchange(true, std::memory_order_acq_rel)) return;
    int fd = shutdown_signal_efd.load(std::memory_order_relaxed);
    if (fd >= 0) (void)::eventfd_write(fd, 1);
}

#include <QApplication>
#include <QMetaObject>
#include <QSocketNotifier>
#include "GUI/Dashboard.hpp"
#include "SystemOptimizer.hpp"

namespace {

void print_selftest_report(const Scalpel::SelfTest::Report& r) {
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
}

} // namespace

static void close_shutdown_signal_efd() {
    int fd = shutdown_signal_efd.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) ::close(fd);
}

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

    {
        int fd = ::eventfd(0, EFD_CLOEXEC);
        if (fd < 0) {
            std::println(stderr, "[Fatal] shutdown eventfd: {}", std::strerror(errno));
            return 1;
        }
        shutdown_signal_efd.store(fd, std::memory_order_release);
    }

    int gui_stop_efd = ::eventfd(0, EFD_CLOEXEC);
    if (gui_stop_efd < 0) {
        std::println(stderr, "[Fatal] gui shutdown chain eventfd: {}", std::strerror(errno));
        close_shutdown_signal_efd();
        return 1;
    }

    Scalpel::App app;

    // 2. Register shutdown signals (handler must not call App::stop)
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 3. Low-level system initialization
    if (auto res = app.init(); !res) {
        std::println(stderr, "[Fatal Error] Initialization failed: {}", res.error());
        ::close(gui_stop_efd);
        gui_stop_efd = -1;
        close_shutdown_signal_efd();
        return 1;
    }

    int ret = 0;
    try {
        if (Scalpel::Config::global_state.enable_gui.load(std::memory_order_relaxed)) {
            // Core 0: UI/Graphics — must be set before QApplication construction
            Scalpel::System::Optimizer::set_current_thread_affinity(0);

            std::atomic<bool> gui_shutdown_runner_quit{false};

            int selftest_done_efd = ::eventfd(0, EFD_CLOEXEC);
            if (selftest_done_efd < 0) {
                std::println(stderr, "[Fatal] selftest done eventfd: {}",
                    std::strerror(errno));
                return 1;
            }

            QApplication qapp(argc, argv);

            std::thread gui_shutdown_runner([&app, gui_stop_efd, &gui_shutdown_runner_quit]() {
                Scalpel::System::Optimizer::set_current_thread_affinity(1);
                for (;;) {
                    uint64_t v;
                    int rr = ::eventfd_read(gui_stop_efd, &v);
                    if (rr < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (gui_shutdown_runner_quit.load(std::memory_order_relaxed)) break;
                    app.stop();
                }
            });

            Scalpel::GUI::Dashboard gui;
            gui.showFullScreen();

            QSocketNotifier shutdown_sn(
                shutdown_signal_efd.load(std::memory_order_relaxed),
                QSocketNotifier::Read, &qapp);
            QObject::connect(&shutdown_sn, &QSocketNotifier::activated, &qapp,
                [gui_stop_efd](int) {
                    uint64_t v;
                    int      fd = shutdown_signal_efd.load(std::memory_order_relaxed);
                    if (fd >= 0) (void)::eventfd_read(fd, &v);
                    (void)::eventfd_write(gui_stop_efd, 1);
                });

            QSocketNotifier selftest_done_sn(selftest_done_efd, QSocketNotifier::Read, &qapp);
            QObject::connect(&selftest_done_sn, &QSocketNotifier::activated, &qapp,
                [&app, selftest_done_efd](int) {
                    uint64_t v;
                    (void)::eventfd_read(selftest_done_efd, &v);
                    const Scalpel::SelfTest::Report r = Scalpel::SelfTest::LAST_REPORT;
                    print_selftest_report(r);
                    app.start();
                    Scalpel::GUI::Dashboard::on_selftest_done(r);
                });

            // Async self-test: worker sets LAST_REPORT then eventfd_write(selftest_done_efd); Core 0
            // handles read + print + app.start() + Dashboard in the QSocketNotifier slot.
            // SelfTest::~SelfTest() joins the worker (see SelfTest.hpp); that runs when `selftest`
            // goes out of scope at the end of this block, after qapp.exec() and watchdog_notify.join(),
            // not when exec() first returns. Until then the std::thread may still be joinable even if run() has completed.
            Scalpel::SelfTest::SelfTest selftest;
            selftest.registerCallback([selftest_done_efd](const Scalpel::SelfTest::Report& r) {
                Scalpel::SelfTest::LAST_REPORT = r;
                (void)::eventfd_write(selftest_done_efd, 1);
            });
            selftest.start();

            // Watchdog thread: when underlying engine receives Ctrl+C (stop set),
            // safely notify Qt to exit GUI. Fixes bug where signal_handler alone couldn't
            // interrupt Qt exec(), causing terminal to hang completely.
            std::thread watchdog_notify([&app, &qapp]() {
                Scalpel::System::Optimizer::set_current_thread_affinity(1); // Core 1: Watchdog
                app.wait_for_shutdown();
                QMetaObject::invokeMethod(&qapp, "quit", Qt::QueuedConnection);
            });

            ret = qapp.exec(); // Block on main event loop

            selftest_done_sn.setEnabled(false);
            if (selftest_done_efd >= 0) {
                ::close(selftest_done_efd);
                selftest_done_efd = -1;
            }

            // If user closed the window (no UNIX signal), request stop on Core 1 runner
            if (!signal_received.exchange(true, std::memory_order_acq_rel))
                (void)::eventfd_write(gui_stop_efd, 1);

            gui_shutdown_runner_quit.store(true, std::memory_order_relaxed);
            (void)::eventfd_write(gui_stop_efd, 1);
            gui_shutdown_runner.join();

            watchdog_notify.join(); // qapp still in scope; thread is done on both exit paths
        } else {
            // Pure CLI mode: dedicated thread reads shutdown eventfd and calls App::stop().
            std::atomic<bool> cli_listener_run{true};
            std::thread cli_shutdown_listener([&app, &cli_listener_run] {
                Scalpel::System::Optimizer::set_current_thread_affinity(1);
                while (cli_listener_run.load(std::memory_order_relaxed)) {
                    uint64_t v;
                    int      fd = shutdown_signal_efd.load(std::memory_order_relaxed);
                    if (fd < 0) break;
                    int      r = ::eventfd_read(fd, &v);
                    if (r < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (!cli_listener_run.load(std::memory_order_relaxed)) break;
                    app.stop();
                }
            });

            {
                Scalpel::SelfTest::SelfTest selftest;
                selftest.registerCallback([](const Scalpel::SelfTest::Report& r) {
                    Scalpel::SelfTest::LAST_REPORT = r;
                });
                selftest.start();
                selftest.join();
                print_selftest_report(Scalpel::SelfTest::LAST_REPORT);
            }
            app.start();
            app.wait_for_shutdown();
            cli_listener_run.store(false, std::memory_order_relaxed);
            {
                int fd = shutdown_signal_efd.load(std::memory_order_relaxed);
                if (fd >= 0) (void)::eventfd_write(fd, 1);
            }
            cli_shutdown_listener.join();
        }
    } catch (const std::exception& e) {
        std::println(stderr, "[Fatal Error] Uncaught exception: {}", e.what());
        if (gui_stop_efd >= 0) {
            ::close(gui_stop_efd);
            gui_stop_efd = -1;
        }
        close_shutdown_signal_efd();
        return 1;
    }

    if (gui_stop_efd >= 0) {
        ::close(gui_stop_efd);
        gui_stop_efd = -1;
    }
    close_shutdown_signal_efd();
    if (Scalpel::Config::SAVE_ON_EXIT.load(std::memory_order_relaxed))
        Scalpel::Config::save_config("config/config.txt");
    std::println("[System] Application cleanly exited. Kernel resources fully released.");

    return ret;
}
