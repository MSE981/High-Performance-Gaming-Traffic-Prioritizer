#include "App.hpp"
#include <csignal>

int main() {
    // 忽略 SIGPIPE 防止由于 Socket 异常导致的程序退出
    signal(SIGPIPE, SIG_IGN);

    NetworkScalpelApp app;
    if (auto res = app.init(); !res) {
        std::print(stderr, "Initialization Failed: {}\n", res.error());
        return 1;
    }

    app.run();
    return 0;
}