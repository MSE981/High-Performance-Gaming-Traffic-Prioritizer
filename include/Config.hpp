#pragma once
#include <string>
#include <cstdint>
#include <fstream>
#include <vector>
#include <print>  
#include <ranges> 

namespace Scalpel::Config {
    // 接口配置 (改为 inline 允许运行时由 Web 后端覆盖)
    inline std::string IFACE_WAN = "eth0";
    inline std::string IFACE_LAN = "eth1"; // USB网卡

    // 启发式阈值
    inline uint32_t LARGE_PACKET_THRESHOLD = 1000;
    inline uint32_t PUNISH_TRIGGER_COUNT = 30;
    inline uint32_t CLEANUP_INTERVAL = 10000;

    // 端口白名单存储结构
    struct PortRange { uint16_t start; uint16_t end; };
    inline std::vector<PortRange> GAME_PORTS = { {3074, 3074}, {27015, 27015}, {12000, 12999} };

    // 游戏端口白名单 (运行时遍历校验)
    inline bool is_game_port(uint16_t port) {
        for (const auto& range : GAME_PORTS) {
            if (port >= range.start && port <= range.end) return true;
        }
        return false;
    }

    // 新增：动态加载配置
    inline void load_config(const std::string& path = "config.txt") {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::println(stderr, "[Config] Warning: Cannot open {}, using default settings.", path);
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
                else if (key == "LARGE_PACKET_THRESHOLD") LARGE_PACKET_THRESHOLD = std::stoul(val);
                else if (key == "PUNISH_TRIGGER_COUNT") PUNISH_TRIGGER_COUNT = std::stoul(val);
                else if (key == "CLEANUP_INTERVAL") CLEANUP_INTERVAL = std::stoul(val);
                else if (key == "GAME_PORTS") {
                    GAME_PORTS.clear();

                    for (const auto& word_range : val | std::views::split(',')) {
                        std::string token(std::ranges::begin(word_range), std::ranges::end(word_range));
                        auto dash = token.find('-');
                        if (dash != std::string::npos) {
                            uint16_t start = static_cast<uint16_t>(std::stoul(token.substr(0, dash)));
                            uint16_t end = static_cast<uint16_t>(std::stoul(token.substr(dash + 1)));
                            GAME_PORTS.push_back({ start, end });
                        }
                        else {
                            uint16_t p = static_cast<uint16_t>(std::stoul(token));
                            GAME_PORTS.push_back({ p, p });
                        }
                    }
                }
            }
            catch (...) {
                std::println(stderr, "[Config] Error parsing line: {}", line);
            }
        }
        std::println("[Config] Loaded configuration from {}", path);
    }
}