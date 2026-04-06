#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <print>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
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
            std::string ip = inet_ntoa(((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr);
            close(fd);
            return ip;
        }

        // 2. Auto-get default gateway IP (from /proc/net/route)
        static std::string get_gateway_ip() {
            std::ifstream route_file("/proc/net/route");
            std::string line;
            while (std::getline(route_file, line)) {
                std::stringstream ss(line);
                std::string iface, dest, gateway;
                ss >> iface >> dest >> gateway;
                // Destination 00000000 means default route
                if (dest == "00000000") {
                    unsigned int addr = std::stoul(gateway, nullptr, 16);
                    struct in_addr in {};
                    in.s_addr = addr;
                    return inet_ntoa(in);
                }
            }
            return "";
        }

        // 3. Auto-get MAC for target IP (from /proc/net/arp)
        static std::string get_mac_from_arp(const std::string& target_ip) {
            // Hint: if no result, may need to ping target IP first to activate ARP cache
            std::ifstream arp_file("/proc/net/arp");
            std::string line;
            std::getline(arp_file, line); // Skip header line
            while (std::getline(arp_file, line)) {
                std::stringstream ss(line);
                std::string ip, hw_type, flags, mac, mask, dev;
                ss >> ip >> hw_type >> flags >> mac >> mask >> dev;
                if (ip == target_ip) return mac;
            }
            return "";
        }
        // 4. Pure C++ ARP activation (replaces inefficient system("ping"))
        static void force_arp_resolution(const std::string& target_ip) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return;

            struct sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(33434); // 使用 traceroute 的默认高端口
            inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr);

            // 发送 1 字节的空 UDP 数据报。
            // Linux 内核协议栈发现目标 MAC 未知时，会自动在底层广播 ARP 请求！0 进程开销！
            char dummy = 0;
            sendto(fd, &dummy, 1, 0, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
            close(fd);
        }

        // 5. 纯 C++ 实现的网卡硬件卸载特性修改 (替代违规的 system("ethtool"))
        static bool disable_hardware_offloads(const std::string& iface) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return false;

            struct ifreq ifr {};
            strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
            ifr.ifr_name[IFNAMSIZ - 1] = '\0';

            struct ethtool_value eval {};
            bool success = true;

            // 通过底层的 SIOCETHTOOL ioctl 指令，直接向网卡驱动下发硬件寄存器修改命令
            auto set_offload = [&](uint32_t cmd) {
                eval.cmd = cmd;
                eval.data = 0; // 0 代表 Disable
                ifr.ifr_data = reinterpret_cast<char*>(&eval);
                if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) success = false;
                };

            set_offload(ETHTOOL_SGRO); // 关闭 GRO
            set_offload(ETHTOOL_SGSO); // 关闭 GSO
            set_offload(ETHTOOL_STSO); // 关闭 TSO
            set_offload(ETHTOOL_SSG);  // 关闭 SG (Scatter-Gather)

            close(fd);
            return success;
        }



    };

}
