#include "App.hpp"
#include "Config.hpp"
#include <csignal>
#include <print>

// 全局指针，用于信号处理
Scalpel::App* global_app = nullptr;

// 信号处理入口
extern "C" void signal_handler(int signal) {
    std::println("\n[System] 收到信号 {}. 正在发起优雅关闭...", signal);
    if (global_app) {
        global_app->stop();
    }
}

#include <QApplication>
#include "GUI/Dashboard.hpp"

int main(int argc, char* argv[]) {
    // 忽略 SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);

    // 1. 加载软路由与系统配置
    Scalpel::Config::load_config();

    Scalpel::App app;
    global_app = &app;

    // 2. 注册退出信号
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 3. 系统底层初始化
    if (auto res = app.init(); !res) {
        std::println(stderr, "[Fatal Error] 初始化失败: {}", res.error());
        return 1;
    }

    try {
        if (Scalpel::Config::global_state.enable_gui) {
            // [阶段 3] 启动 Qt GUI 模式 (Core 0 将用于 QEventLoop)
            QApplication qapp(argc, argv);
            Scalpel::GUI::Dashboard gui;
            
            app.start(); // 异步启动底层网络引擎
            gui.show();
            
            int ret = qapp.exec(); // 阻塞在主事件循环，不污染底层数据刷新
            app.stop();
            return ret;
        } else {
            // 纯命令行模式，阻塞等待退出
            app.start();
            app.wait_for_shutdown();
        }
    } catch (const std::exception& e) {
        std::println(stderr, "[Fatal Error] 未俘获异常: {}", e.what());
        return 1;
    }

    global_app = nullptr;
    std::println("[System] 应用程序已清理退出。内核资源已完全释放。");

    return 0;
}
