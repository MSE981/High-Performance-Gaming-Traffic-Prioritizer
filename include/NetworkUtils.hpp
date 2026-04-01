#pragma once
#include <string>
#include <print>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>


namespace Scalpel::Utils {
    class Network {
    public:
        // 1. Auto-get local IP for specified interface
        static std::string get_local_ip(const std::string& iface) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return "127.0.0.1"; // Intercept failed fd, prevent crash on subsequent syscalls

            struct ifreq ifr {};
            ifr.ifr_addr.sa_family = AF_INET;

            // GNU++23 defensive: eliminate GCC-14 string truncation warning, ensure null termination
            strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
            ifr.ifr_name[IFNAMSIZ - 1] = '\0';

            if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
                close(fd);
                return "127.0.0.1";
            }
            char ip_buf[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr, ip_buf, sizeof(ip_buf));
            close(fd);
            return ip_buf;
        }

        // 2. Auto-get default gateway IP (raw fd, no ifstream)
        static std::string get_gateway_ip() {
            char buf[2048]{};
            int fd = ::open("/proc/net/route", O_RDONLY);
            if (fd < 0) return "";
            ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
            ::close(fd);
            if (n <= 0) return "";

            // Parse line by line: skip header, find dest==00000000
            char* line = buf;
            char* end  = buf + n;
            // Skip header line
            while (line < end && *line != '\n') ++line;
            if (line < end) ++line;

            while (line < end) {
                char iface[32]{}, dest[16]{}, gw[16]{};
                if (sscanf(line, "%31s %15s %15s", iface, dest, gw) == 3) {
                    if (strcmp(dest, "00000000") == 0) {
                        unsigned int addr = 0;
                        if (sscanf(gw, "%x", &addr) == 1) {
                            struct in_addr in{};
                            in.s_addr = addr;
                            char gw_buf[INET_ADDRSTRLEN]{};
                            inet_ntop(AF_INET, &in, gw_buf, sizeof(gw_buf));
                            return gw_buf;
                        }
                    }
                }
                while (line < end && *line != '\n') ++line;
                if (line < end) ++line;
            }
            return "";
        }

        // 3. Auto-get MAC for target IP (raw fd, no ifstream)
        static std::string get_mac_from_arp(const std::string& target_ip) {
            char buf[4096]{};
            int fd = ::open("/proc/net/arp", O_RDONLY);
            if (fd < 0) return "";
            ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
            ::close(fd);
            if (n <= 0) return "";

            // Skip header line
            char* line = buf;
            char* end  = buf + n;
            while (line < end && *line != '\n') ++line;
            if (line < end) ++line;

            while (line < end) {
                char ip[32]{}, hw[8]{}, flags[8]{}, mac[20]{};
                if (sscanf(line, "%31s %7s %7s %19s", ip, hw, flags, mac) >= 4) {
                    if (target_ip == ip) return mac;
                }
                while (line < end && *line != '\n') ++line;
                if (line < end) ++line;
            }
            return "";
        }
        // 4. Pure C++ ARP activation (replaces inefficient system("ping"))
        static void force_arp_resolution(const std::string& target_ip) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return;

            struct sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(33434); // default traceroute high port
            inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr);

            // Send a 1-byte dummy UDP datagram.
            // Kernel auto-broadcasts ARP request when dest MAC is unknown — zero process overhead.
            char dummy = 0;
            sendto(fd, &dummy, 1, 0, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
            close(fd);
        }

        // 5. Disable NIC hardware offloads via ioctl (replaces system("ethtool"))
        static bool disable_hardware_offloads(const std::string& iface) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return false;

            struct ifreq ifr {};
            strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
            ifr.ifr_name[IFNAMSIZ - 1] = '\0';

            struct ethtool_value eval {};
            bool success = true;

            // Issue SIOCETHTOOL ioctl to directly modify NIC hardware register
            auto set_offload = [&](uint32_t cmd) {
                eval.cmd = cmd;
                eval.data = 0; // 0 = disable
                ifr.ifr_data = reinterpret_cast<char*>(&eval);
                if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) success = false;
                };

            set_offload(ETHTOOL_SGRO); // disable GRO
            set_offload(ETHTOOL_SGSO); // disable GSO
            set_offload(ETHTOOL_STSO); // disable TSO
            set_offload(ETHTOOL_SSG);  // disable SG (Scatter-Gather)

            close(fd);
            return success;
        }



    };

}