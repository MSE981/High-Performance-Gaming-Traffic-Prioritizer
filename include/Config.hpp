#pragma once
#include <string>
#include <cstdint>
#include <fstream>
#include <array>
#include <map>
#include <vector>
#include <print>  
#include <ranges> 

namespace Scalpel::Config {
    // 动态运行时开关状态 (遵循 RCU/锁屏障概念，用于数据面 O(1) 获取特性状态)
    struct DynamicState {
        std::atomic<bool> enable_nat{true};
        std::atomic<bool> enable_dhcp{true};
        std::atomic<bool> enable_dns_cache{true};
        std::atomic<bool> enable_upnp{true};
        std::atomic<bool> enable_firewall{true};
        std::atomic<bool> enable_pppoe{false};
        std::atomic<bool> enable_gui{true};
    };
    inline DynamicState global_state;

    // 接口配置 (允许运行时由 Web 端或配置文件覆盖)
    inline std::string IFACE_WAN = "eth0";
    inline std::string IFACE_LAN = "eth1"; // 局域网/USB网卡接口
    inline std::atomic<bool> ENABLE_ACCELERATION{true}; // 加速/透明网桥开关

    // 桥接深度配置
    inline std::atomic<bool> ENABLE_STP{false};
    inline std::atomic<bool> ENABLE_IGMP_SNOOPING{false};
    inline std::vector<std::string> BRIDGED_INTERFACES = {"eth1", "eth2", "eth3"};

    // 启发式检测算法阈值
    inline uint32_t LARGE_PACKET_THRESHOLD = 1000;
    inline uint32_t PUNISH_TRIGGER_COUNT = 30;
    inline uint32_t CLEANUP_INTERVAL = 10000;

    // 游戏协议端口白名单 (默认值)
    struct PortRange { uint16_t start; uint16_t end; };
    inline std::array<PortRange, 3> GAME_PORTS = {{ {3074, 3074}, {27015, 27015}, {12000, 12999} }};

    // 软路由功能：特定终端 IP 限速表
    inline std::map<uint32_t, double> IP_LIMIT_MAP;

    // 辅助工具：判断是否为游戏端口
    inline bool is_game_port(uint16_t port) {
        for (const auto& range : GAME_PORTS) {
            if (port >= range.start && port <= range.end) return true;
        }
        return false;
    }

    // 辅助工具：将字符串 IP (A.B.C.D) 解析为网络序 uint32
    inline uint32_t parse_ip_str(const std::string& ip_str) {
        uint32_t a, b, c, d;
        if (sscanf(ip_str.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
             return (a << 0) | (b << 8) | (c << 16) | (d << 24); // 简单大小端转换取决于具体架构要求
        }
        return 0;
    }

    // 从 config.txt 加载系统动态配置
    inline void load_config(const std::string& path = "config.txt") {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::println(stderr, "[Config] 警告: 无法打开配置文件 {}, 使用系统默认值。", path);
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line.starts_with('#')) continue;

            auto delim = line.find('=');
            if (delim == std::string::npos) continue;

            std::string key = line.substr(0, delim);
            std::string val = line.substr(delim + 1);

            try {
                if (key == "IFACE_WAN") IFACE_WAN = val;
                else if (key == "IFACE_LAN") IFACE_LAN = val;
                else if (key == "ENABLE_ACCELERATION") ENABLE_ACCELERATION = (val == "true" || val == "1");
                else if (key == "LARGE_PACKET_THRESHOLD") LARGE_PACKET_THRESHOLD = std::stoul(val);
                else if (key == "PUNISH_TRIGGER_COUNT") PUNISH_TRIGGER_COUNT = std::stoul(val);
                else if (key == "CLEANUP_INTERVAL") CLEANUP_INTERVAL = std::stoul(val);
                else if (key == "enable_gui") global_state.enable_gui.store(val == "true" || val == "1", std::memory_order_relaxed);
                else if (key == "IP_LIMIT") {
                    auto dash = val.find(':');
                    if (dash != std::string::npos) {
                        uint32_t ip = parse_ip_str(val.substr(0, dash));
                        double limit = std::stod(val.substr(dash + 1));
                        IP_LIMIT_MAP[ip] = limit;
                    }
                }
            } catch (...) {
                std::println(stderr, "[Config] 错误: 无法解析配置行: {}", line);
            }
        }
        std::println("[Config] 配置文件加载完成: {}", path);
    }
}


