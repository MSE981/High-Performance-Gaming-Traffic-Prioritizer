#pragma once
#include <string_view>
#include <cstdint>

namespace Scalpel::Config {
    // 接口配置
    constexpr std::string_view IFACE_WAN = "eth0";
    constexpr std::string_view IFACE_LAN = "eth1"; // USB网卡

    // 启发式阈值
    constexpr uint32_t LARGE_PACKET_THRESHOLD = 1000; // 超过此大小视为大包
    constexpr uint32_t PUNISH_TRIGGER_COUNT = 20;     // 前N个包中有15个大包则降级
    constexpr uint32_t CLEANUP_INTERVAL = 5000;       // 处理多少个包清理一次流表

    // 游戏端口白名单 (示例)
    constexpr bool is_game_port(uint16_t port) {
        return port == 3074 || port == 27015 || (port >= 12000 && port <= 12999);
    }
}