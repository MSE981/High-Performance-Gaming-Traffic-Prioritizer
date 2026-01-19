#pragma once
#include <string>
#include <expected>
#include <memory>
#include <span>
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <sys/mman.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>

class RawSocketManager {
    int fd = -1;
    std::string iface;
    std::uint8_t* ring = nullptr;
    std::size_t ring_size = 0;

    static constexpr std::uint32_t BLOCK_SIZE = 4096 * 8; 
    static constexpr std::uint32_t FRAME_SIZE = 2048;     
    static constexpr std::uint32_t BLOCK_NR   = 64;       
    static constexpr std::uint32_t FRAME_NR   = (BLOCK_SIZE * BLOCK_NR) / FRAME_SIZE;

public:
    explicit RawSocketManager(std::string name) : iface(std::move(name)) {}
    ~RawSocketManager() {
        if (ring) munmap(ring, ring_size);
        if (fd >= 0) close(fd);
    }

    std::expected<void, std::string> open() {
        fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (fd < 0) return std::unexpected("Socket failed: " + std::string(strerror(errno)));

        struct ifreq ifr{};
        std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ);
        if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) return std::unexpected("IF lookup failed");

        tpacket_req req{.tp_block_size = BLOCK_SIZE, .tp_block_nr = BLOCK_NR, 
                        .tp_frame_size = FRAME_SIZE, .tp_frame_nr = FRAME_NR};

        if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0)
            return std::unexpected("RX_RING failed");

        ring_size = static_cast<std::size_t>(req.tp_block_size) * req.tp_block_nr;
        ring = static_cast<std::uint8_t*>(mmap(nullptr, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        
        sockaddr_ll sll{.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = ifr.ifr_ifindex};
        if (bind(fd, reinterpret_cast<sockaddr*>(&sll), sizeof(sll)) < 0) return std::unexpected("Bind failed");

        return {};
    }

    [[nodiscard]] int get_fd() const { return fd; }
    [[nodiscard]] std::uint8_t* get_ring() const { return ring; }
    static constexpr std::uint32_t frame_size() { return FRAME_SIZE; }
    static constexpr std::uint32_t frame_nr() { return FRAME_NR; }
};