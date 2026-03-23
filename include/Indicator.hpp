#pragma once
#include <fstream>
#include <string>

namespace Scalpel::HW {
    class RGBLed {
        // BCM GPIO Nums
        static constexpr auto RED = "17";
        static constexpr auto GREEN = "27";
        static constexpr auto YELLOW = "22"; // 假设 Blue 接在 22, 混色逻辑略

        void write_sysfs(const std::string& pin, const std::string& val) {
            std::ofstream f("/sys/class/gpio/gpio" + pin + "/value");
            if (f.is_open()) f << val;
        }

    public:
        RGBLed() { /* Initialize export/direction in constructor or setup script */ }

        void set_yellow() { write_sysfs(RED, "1"); write_sysfs(GREEN, "1"); }
        void set_green() { write_sysfs(RED, "0"); write_sysfs(GREEN, "1"); }
        void set_red() { write_sysfs(RED, "1"); write_sysfs(GREEN, "0"); }
        void off() { write_sysfs(RED, "0"); write_sysfs(GREEN, "0"); }
    };
}