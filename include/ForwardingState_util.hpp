#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include "Types_util.hpp"

namespace HPGTP::ForwardState_util {

// Owns the double-buffered L2/ARP snapshot used for Ethernet rewrite and the
// kernel WAN address lookup. 
class ForwardingState_util {
public:
    struct ArpEntry {
        uint32_t ip_nbo = 0;
        std::array<uint8_t, 6> mac{};
    };

    struct L2Snapshot {
        std::array<uint8_t, 6> lan_hw{};
        std::array<uint8_t, 6> wan_hw{};
        std::array<uint8_t, 6> gw_hw{};
        uint32_t wan_ip_nbo = 0;
        int32_t wan_prefix_len = -1;
        static constexpr size_t MAX_ARP = 128;
        std::array<ArpEntry, MAX_ARP> arp{};
        uint32_t arp_count = 0;
        bool gw_hw_valid = false;
        bool wan_cfg_valid = false;
        bool default_gw_ip_configured = false;
        bool ready = false;
    };

    ForwardingState_util() = default;

    std::expected<Net::IPv4Net, std::string> resolve_nat_wan_ip() const;
    void refresh_l2();
    const L2Snapshot& snapshot() const;
    bool resolve_mac_onlink(const L2Snapshot& snap, uint32_t dst_ip_nbo,
                            uint8_t out_mac[6]) const;

private:
    static bool is_invalid_nat_wan(Net::IPv4Net a) noexcept;
    std::array<L2Snapshot, 2> snap_{};
    std::atomic<unsigned> active_{0};
};

} // namespace HPGTP::ForwardState_util
