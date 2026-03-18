#pragma once
#include <string>
#include <expected>
#include <span>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <functional>
#include <poll.h>   

namespace Scalpel::Engine {
    class RawSocketManager {
        int fd = -1;
        uint8_t* ring = nullptr;
        size_t ring_size = 0;

        uint32_t rx_idx = 0; // 核心封装：将环形缓冲区索引隐藏在底层，拒绝应用层轮询
        std::function<void(std::span<const uint8_t>)> onPacketReceived; // 核心封装：声明事件回调函数

        // TPACKET_V1/V2 默认配置
		static constexpr uint32_t BLOCK_SIZE = 4096 * 16; // 4K * 816 = 32768 bytes
        static constexpr uint32_t FRAME_SIZE = 2048;
        // 修复：将内核 RX Ring 扩大 8 倍，彻底接住 Bing 等网页的突发大数据流
        static constexpr uint32_t BLOCK_NR = 1024;
        static constexpr uint32_t FRAME_NR = (BLOCK_SIZE * BLOCK_NR) / FRAME_SIZE;

    public:
        explicit RawSocketManager(std::string_view iface_name) : iface(iface_name) {}

        ~RawSocketManager() {
            // 修复：mmap 失败时返回 MAP_FAILED 而不是 nullptr
            if (ring && ring != MAP_FAILED) munmap(ring, ring_size);
            if (fd >= 0) close(fd);
        }

        std::expected<void, std::string> init() {
            // 1. 创建 Raw Socket
            fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
            if (fd < 0) return std::unexpected(std::string("Socket creation failed: ") + strerror(errno));

            // 2. 获取接口 Index
            struct ifreq ifr {};
            iface.copy(ifr.ifr_name, IFNAMSIZ - 1);
            if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) return std::unexpected("Interface lookup failed");

            // --- 自动开启网卡混杂模式逻辑 ---
            struct ifreq ifr_p {};
            iface.copy(ifr_p.ifr_name, IFNAMSIZ - 1);

            // 获取当前网卡标志位
            if (ioctl(fd, SIOCGIFFLAGS, &ifr_p) < 0) {
                return std::unexpected("Failed to get interface flags for promisc mode");
            }

            // 加上混杂模式位并写回内核
            ifr_p.ifr_flags |= IFF_PROMISC;
            if (ioctl(fd, SIOCSIFFLAGS, &ifr_p) < 0) {
                // 如果这里报错，通常是因为没加 CAP_NET_ADMIN 权限
                return std::unexpected("Failed to set IFF_PROMISC. Check sudo/setcap permissions.");
            }
            std::println("[Engine] Promiscuous mode enabled on {}", iface);

            // 3. 配置 PACKET_RX_RING
            tpacket_req req{
                .tp_block_size = BLOCK_SIZE,
                .tp_block_nr = BLOCK_NR,
                .tp_frame_size = FRAME_SIZE,
                .tp_frame_nr = FRAME_NR
            };

            if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0)
                return std::unexpected("Setsockopt RX_RING failed");

            // 4. mmap 映射
            ring_size = (size_t)req.tp_block_size * req.tp_block_nr;
            ring = (uint8_t*)mmap(nullptr, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (ring == MAP_FAILED) return std::unexpected("mmap failed");

            // 5. Bind
            sockaddr_ll sll{};
            sll.sll_family = AF_PACKET;
            sll.sll_protocol = htons(ETH_P_ALL);
            sll.sll_ifindex = ifr.ifr_ifindex;
            if (bind(fd, (sockaddr*)&sll, sizeof(sll)) < 0) return std::unexpected("Bind failed");

            return {};
        }

        int get_fd() const { return fd; }
        uint8_t* get_ring() const { return ring; }
        static constexpr uint32_t frame_size() { return FRAME_SIZE; }
        static constexpr uint32_t frame_nr() { return FRAME_NR; }

    private:
        std::string iface;
    };
}