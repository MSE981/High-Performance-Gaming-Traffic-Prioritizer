#include "App.hpp"
#include <csignal>
#include <print>

int main() {
    // 忽略 SIGPIPE 防止 Socket 写入错误导致退出
    std::signal(SIGPIPE, SIG_IGN);

    Scalpel::App app;

    // 初始化
    if (auto res = app.init(); !res) {
        std::println(stderr, "Fatal Error: {}", res.error());
        return 1;
    }

    try {
        app.run();
    }
    catch (const std::exception& e) {
        std::println(stderr, "Unhandled Exception: {}", e.what());
        return 1;
    }

    return 0;
}