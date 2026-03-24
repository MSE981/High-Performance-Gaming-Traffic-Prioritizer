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

int main() {
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
        // 4. 启动多核异构事件循环
        app.run();
    } catch (const std::exception& e) {
        std::println(stderr, "[Fatal Error] 未俘获异常: {}", e.what());
        return 1;
    }

    global_app = nullptr;
    std::println("[System] 应用程序已清理退出。内核资源已完全释放。");

    return 0;
}
