#include "App.hpp"
#include "Config.hpp"
#include <csignal>
#include <print>

// 全局指针与信号防抖标记
Scalpel::App* global_app = nullptr;
std::atomic<bool> signal_received{false};

// 信号处理入口
extern "C" void signal_handler(int signal) {
    if (signal_received.exchange(true)) return; // 拦截二次退出
    
    // 注意：不要在异步 UNIX 信号里直接执行重度逻辑，仅放行底层关闭
    if (global_app) {
        global_app->stop();
    }
}

#include <QApplication>
#include <QMetaObject>
#include <thread>
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
            // [阶段 3] 启动 Qt GUI 模式
            QApplication qapp(argc, argv);
            Scalpel::GUI::Dashboard gui;
            
            app.start(); // 异步启动底层网络引擎
            gui.show();
            
            // 守护线程：当底层引擎收到 Ctrl+C (stop_promise被设置) 完毕后，安全通知 Qt 退出界面。
            // 解决此前单纯的 signal_handler 无法中断 Qt exec() 导致终端被完全卡死的恶性 Bug！
            std::jthread qt_stopper([&app, &qapp]() {
                app.wait_for_shutdown();
                QMetaObject::invokeMethod(&qapp, "quit", Qt::QueuedConnection);
            });
            
            int ret = qapp.exec(); // 阻塞在主事件循环
            
            // 如果是用户点击窗口 X 号主动关闭了 GUI，此时并未产生 UNIX 信号，我们必须反向告知底层结束。
            if (!signal_received.exchange(true)) {
                app.stop(); 
            }
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
