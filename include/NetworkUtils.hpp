#pragma once
#include <string>

namespace HPGTP::Utils {
    class Network {
    public:
        // Returns dotted IPv4 or empty string if the interface has no address or ioctl fails.
        static std::string get_local_ip(const std::string& iface);
        static std::string get_gateway_ip();
        static std::string get_mac_from_arp(const std::string& target_ip);
        static void        force_arp_resolution(const std::string& target_ip);
        static bool        disable_hardware_offloads(const std::string& iface);
    };
}
