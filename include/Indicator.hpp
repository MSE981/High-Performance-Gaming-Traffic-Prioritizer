#pragma once
#include <string>
#include <print>
#include <memory>
#include <gpiod.hpp>

namespace Scalpel::HW {
    class RGBLed {
        std::shared_ptr<gpiod::line_request> request;
        static constexpr int PIN_RED = 17;
        static constexpr int PIN_GREEN = 27;
        static constexpr int PIN_YELLOW = 22; // 如有需要可后续加入

    public:
        RGBLed() {
            try {

                auto chip = std::make_shared<gpiod::chip>("/dev/gpiochip4");

                gpiod::line_config line_cfg;

                // 配置 RED 和 GREEN 引脚为输出模式
                line_cfg.add_line_settings(
                    PIN_RED,
                    gpiod::line_settings().set_direction(gpiod::line::direction::OUTPUT)
                );
                line_cfg.add_line_settings(
                    PIN_GREEN,
                    gpiod::line_settings().set_direction(gpiod::line::direction::OUTPUT)
                );

                auto builder = chip->prepare_request();
                builder.set_consumer("GamingTrafficPrioritizer_Watchdog");
                builder.set_line_config(line_cfg);

                // 发起请求，底层完成内存映射并锁定这两个引脚
                request = std::make_shared<gpiod::line_request>(builder.do_request());

                off(); // 初始状态关闭
            }
            catch (const std::exception& e) {
                // 异常保护：如果未以 sudo 运行或芯片号不对，确保主程序不会崩溃
                std::println(stderr, "[HW] Warning: libgpiod init failed. LEDs disabled. ({})", e.what());
            }
        }

        // 极速内存操作，没有任何文件 I/O 开销！
        void set_yellow() { set_pins(gpiod::line::value::ACTIVE, gpiod::line::value::ACTIVE); }
        void set_green() { set_pins(gpiod::line::value::INACTIVE, gpiod::line::value::ACTIVE); }
        void set_red() { set_pins(gpiod::line::value::ACTIVE, gpiod::line::value::INACTIVE); }
        void off() { set_pins(gpiod::line::value::INACTIVE, gpiod::line::value::INACTIVE); }

    private:
        void set_pins(gpiod::line::value red_val, gpiod::line::value green_val) {
            if (!request) return;
            try {
                request->set_value(PIN_RED, red_val);
                request->set_value(PIN_GREEN, green_val);
            }
            catch (...) {}
        }
    };
}