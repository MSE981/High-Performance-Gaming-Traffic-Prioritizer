#include "App.hpp"
#include <csignal>
#include <print>

// 声明全局指针，以便 C 风格的信号处理函数能够访问到 C++ 的 app 实例
Scalpel::App* global_app = nullptr;

// 实现退出与资源回收
extern "C" void signal_handler(int signal) {
    std::println("\n[System] Received signal {}. Initiating graceful shutdown...", signal);
    if (global_app) {
        global_app->stop(); // 触发 App.hpp 中的 promise，解锁主线程
    }
}

int main() {
    // 忽略 SIGPIPE 防止 Socket 写入错误导致退出
    std::signal(SIGPIPE, SIG_IGN);

    Scalpel::App app;
    global_app = &app;

    // 注册 SIGINT (Ctrl+C) 和 SIGTERM (kill) 信号，接管程序的生死控制权
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 初始化
    if (auto res = app.init(); !res) {
        std::println(stderr, "Fatal Error: {}", res.error());
        return 1;
    }

    try {
        app.run(); // 主线程将在此处挂起
    }
    catch (const std::exception& e) {
        std::println(stderr, "Unhandled Exception: {}", e.what());
        return 1;
    }

    // 当 Ctrl+C 被按下，app.run() 结束阻塞并返回，局部变量 app 准备销毁
    // 此时 C++23 的 jthread 会自动给所有 worker 发送 stop_token 并 join() 安全合并
    // RawSocketManager 会安全执行 munmap 和 close
    global_app = nullptr;
    std::println("[System] Application exited cleanly. Kernel resources fully released.");

    return 0;
}