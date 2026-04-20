#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include "NetworkTypes.hpp"

namespace HPGTP::Utils {

    // One row from /proc/net/arp (IPv4 NBO + Ethernet). Filled by read_arp_table.
    struct ArpTableRow {
        uint32_t ip_nbo = 0;
        uint8_t  mac[6]{};
    };

    class Network {
    public:
        // Returns dotted IPv4 or empty string if the interface has no address or ioctl fails.
        static std::string get_local_ip(const std::string& iface);
        // Prefix length 0–32 from SIOCGIFNETMASK; -1 if ioctl fails or mask is non-contiguous.
        static int         get_iface_ipv4_prefix_len(const std::string& iface);
        // Default route (0.0.0.0/0) gateway from /proc/net/route — first matching line (legacy).
        static std::string get_gateway_ip();
        // Default route gateway IP for a specific interface name (WAN).
        static std::string get_default_gateway_for_iface(const std::string& iface);
        static std::string get_mac_from_arp(const std::string& target_ip);
        static void        force_arp_resolution(const std::string& target_ip);
        static bool        disable_hardware_offloads(const std::string& iface);
        // Ethernet MAC via SIOCGIFHWADDR; returns false on failure.
        static bool        get_iface_hwaddr(const std::string& iface, uint8_t out[6]);
        // Parses "aa:bb:cc:dd:ee:ff" from /proc/net/arp; returns false if invalid.
        static bool        parse_mac_colon(std::string_view s, uint8_t out[6]);
        // Single read of /proc/net/arp; skips incomplete entries (flags 0x0). Returns count written.
        static size_t      read_arp_table(ArpTableRow* out, size_t max_out);
        // Assign primary IPv4 + netmask to iface (ioctl). Requires CAP_NET_ADMIN / root.
        // Returns false on failure (caller may inspect errno).
        static bool        set_iface_ipv4_and_prefix(const std::string& iface,
            const std::string& ipv4_dotted, int prefix_len);
        // Smallest prefix length (1–32) so that pool_start and pool_end share one network; 32 if equal.
        static int         infer_prefix_covering_pool(Net::IPv4Net pool_start, Net::IPv4Net pool_end) noexcept;
        // True if ip is in the subnet defined by anchor and prefix_len (host bits masked).
        static bool        ipv4_in_subnet(Net::IPv4Net ip, int prefix_len, Net::IPv4Net anchor) noexcept;
    };
}
