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
        static constexpr int PIN_YELLOW = 22; // Can add later if needed

    public:
        RGBLed() {
            try {

                auto chip = std::make_shared<gpiod::chip>("/dev/gpiochip4");

                gpiod::line_config line_cfg;

                // Configure RED and GREEN pins as output
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

                // Request pins: kernel maps memory and locks these two lines
                request = std::make_shared<gpiod::line_request>(builder.do_request());

                off(); // Initial state: off
            }
            catch (const std::exception& e) {
                // Error protection: ensure main program doesn't crash if not running as sudo or chip number wrong
                std::println(stderr, "[HW] Warning: libgpiod init failed. LEDs disabled. ({})", e.what());
            }
        }

        // Ultra-fast memory operations, no file I/O overhead!
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
