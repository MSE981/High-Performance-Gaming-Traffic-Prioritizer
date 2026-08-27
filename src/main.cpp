#include "App.hpp"
#include "Config.hpp"
#include "Telemetry.hpp"
#include "SelfTest.hpp"
#include <csignal>
#include <print>
#include <thread>
#include <QApplication>
#include <QMetaObject>
#include "GUI/Dashboard.hpp"
#include "SystemOptimizer_util.hpp"

namespace {

void print_selftest_report(const HPGTP::SelfTest::Report& r) {
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

int main(int argc, char* argv[]) {
    // Ignore SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
    ::setenv("QT_QPA_EGLFS_HIDECURSOR","1",0);
    ::setenv("QT_QPA_EGLFS_KMS_CONFIG","config/kms.json",0);

    // Load router and system config
    if (auto cr = HPGTP::Config::load_config("config/config.txt"); !cr) {
        std::println(stderr, "[Fatal] {}", cr.error());
        return 1;
    }

    HPGTP::App app;

    // system initialization
    if (auto res = app.init(); !res) {
        std::println(stderr, "[Fatal Error] Initialization failed: {}", res.error());
        return 1;
    }

    int ret = 0;
    try {
        // Core 0+1: UI/graphics thread; must be set before QApplication construction
        HPGTP::System::Optimizer_util::set_current_thread_affinity_control(); 

        QApplication qapp(argc, argv);

        // Lifecycle is driven only by the GUI shutdown button.
        HPGTP::GUI::Dashboard gui(
            [&app]() { app.stop(); },
            [&app]() { app.request_dhcp_config_apply(); });
        gui.showFullScreen();

        // Sync self-test: services start only after every check has run.
        {
            HPGTP::SelfTest::SelfTest selftest;
            HPGTP::SelfTest::Report selftest_report{};
            selftest.registerCallback([&selftest_report](const HPGTP::SelfTest::Report& r) {
                selftest_report = r;
            });
            selftest.start();
            selftest.join();
            print_selftest_report(selftest_report);
        }
        app.start();

        // Watches App shutdown completion and exits the GUI event loop 
        std::thread watchdog_notify([&app, &qapp]() {
            HPGTP::System::Optimizer_util::set_current_thread_affinity_control();
            app.wait_for_shutdown();
            QMetaObject::invokeMethod(&qapp, "quit", Qt::QueuedConnection);
        });

        ret = qapp.exec(); // Block on main event loop

        app.stop();
        watchdog_notify.join();
    } catch (const std::exception& e) {
        std::println(stderr, "[Fatal Error] Uncaught exception: {}", e.what());
        return 1;
    }

    if (HPGTP::Config::SAVE_ON_EXIT.load(std::memory_order_relaxed)) {
        if (auto sr = HPGTP::Config::save_config("config/config.txt"); !sr)
            std::println(stderr, "[Warning] {}", sr.error());
    }
    std::println("[System] Application cleanly exited. Kernel resources fully released.");

    return ret;
}
