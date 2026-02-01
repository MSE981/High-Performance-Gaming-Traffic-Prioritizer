#pragma once
#include <string>
#include <expected> // C++23: std::expected
#include <span>     // C++20/23: std::span
#include <cstring>  // strerror
#include <cerrno>   // errno
#include <unistd.h> // close
#include <sys/socket.h>      // socket(), bind(), setsockopt()
#include <sys/mman.h>        // mmap(), munmap()
#include <sys/ioctl.h>       // ioctl()
#include <linux/if_packet.h> // AF_PACKET, tpacket_req
#include <net/ethernet.h>    // ETH_P_ALL
#include <net/if.h>          // ifreq, IFNAMSIZ
#include <arpa/inet.h>       // htons() 修复之前的报错

namespace Scalpel::Engine {
    class RawSocketManager {
        int fd = -1;
        uint8_t* ring = nullptr;
        size_t ring_size = 0;

        // TPACKET_V1/V2 默认配置
        static constexpr uint32_t BLOCK_SIZE = 4096 * 8;
        static constexpr uint32_t FRAME_SIZE = 2048;
        static constexpr uint32_t BLOCK_NR = 64;
        static constexpr uint32_t FRAME_NR = (BLOCK_SIZE * BLOCK_NR) / FRAME_SIZE;

    public:
        explicit RawSocketManager(std::string_view iface_name) : iface(iface_name) {}

        ~RawSocketManager() {
            if (ring) munmap(ring, ring_size);
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