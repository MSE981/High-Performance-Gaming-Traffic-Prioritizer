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

namespace Scalpel::Utils {
    class Network {
    public:
        // 1. 自动获取指定网卡的本地 IP
        static std::string get_local_ip(const std::string& iface) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return "127.0.0.1"; // 拦截创建失败的 fd，防止后续系统调用崩溃

            struct ifreq ifr {};
            ifr.ifr_addr.sa_family = AF_INET;

            // GNU++23 防御性编程：消除 GCC-14 的字符串截断警告，确保绝对 '\0' 结尾
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

        // 2. 自动获取默认网关 IP (从 /proc/net/route)
        static std::string get_gateway_ip() {
            std::ifstream route_file("/proc/net/route");
            std::string line;
            while (std::getline(route_file, line)) {
                std::stringstream ss(line);
                std::string iface, dest, gateway;
                ss >> iface >> dest >> gateway;
                // Destination 00000000 代表默认路由
                if (dest == "00000000") {
                    unsigned int addr = std::stoul(gateway, nullptr, 16);
                    struct in_addr in {};
                    in.s_addr = addr;
                    return inet_ntoa(in);
                }
            }
            return "";
        }

        // 3. 自动获取指定 IP 对应的 MAC 地址 (从 /proc/net/arp)
        static std::string get_mac_from_arp(const std::string& target_ip) {
            // 提示：如果获取不到，可能需要先 ping 一下目标 IP 激活 ARP 缓存
            std::ifstream arp_file("/proc/net/arp");
            std::string line;
            std::getline(arp_file, line); // 跳过标题行
            while (std::getline(arp_file, line)) {
                std::stringstream ss(line);
                std::string ip, hw_type, flags, mac, mask, dev;
                ss >> ip >> hw_type >> flags >> mac >> mask >> dev;
                if (ip == target_ip) return mac;
            }
            return "";
        }
    };
}