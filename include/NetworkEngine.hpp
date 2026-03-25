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
#include <poll.h>   
#include <print>  

namespace Scalpel::Engine {
    class RawSocketManager {

        // No copy
        RawSocketManager(const RawSocketManager&) = delete;
        RawSocketManager& operator=(const RawSocketManager&) = delete;

        int fd = -1;
        uint8_t* ring = nullptr;
        size_t ring_size = 0;

        uint32_t rx_idx = 0; // Ring buffer index

        // TPACKET_V1/V2 default configuration
        static constexpr uint32_t BLOCK_SIZE = 4096 * 16;
        static constexpr uint32_t FRAME_SIZE = 2048;
        static constexpr uint32_t BLOCK_NR = 1024;
        static constexpr uint32_t FRAME_NR = (BLOCK_SIZE * BLOCK_NR) / FRAME_SIZE;

    public:
        explicit RawSocketManager(std::string_view iface_name) : iface(iface_name) {}

        ~RawSocketManager() {
            if (ring && ring != MAP_FAILED) munmap(ring, ring_size);
            if (fd >= 0) close(fd);
        }

        std::expected<void, std::string> init() {
            // 1. Create raw socket
            fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
            if (fd < 0) return std::unexpected(std::string("Socket creation failed: ") + strerror(errno));

            // 2. Get interface index
            struct ifreq ifr {};
            iface.copy(ifr.ifr_name, IFNAMSIZ - 1);
            if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) return std::unexpected("Interface lookup failed");

            // Auto-enable promiscuous mode
            struct ifreq ifr_p {};
            iface.copy(ifr_p.ifr_name, IFNAMSIZ - 1);

            if (ioctl(fd, SIOCGIFFLAGS, &ifr_p) < 0) {
                return std::unexpected("Failed to get interface flags");
            }

            ifr_p.ifr_flags |= IFF_PROMISC;
            if (ioctl(fd, SIOCSIFFLAGS, &ifr_p) < 0) {
                return std::unexpected("Failed to set IFF_PROMISC. Check permissions.");
            }
            std::println("[Engine] Interface {} set to promiscuous mode", iface);

            // 3. Configure PACKET_RX_RING
            tpacket_req req{
                .tp_block_size = BLOCK_SIZE,
                .tp_block_nr = BLOCK_NR,
                .tp_frame_size = FRAME_SIZE,
                .tp_frame_nr = FRAME_NR
            };

            if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0)
                return std::unexpected("Setsockopt RX_RING failed");

            // 4. Memory map
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

        // Core packet dispatch loop (compile-time polymorphism optimization)
        // Template parameters achieve 100% inlining of processing logic, eliminating virtual function overhead
        template<typename Callback>
        void poll_and_dispatch(Callback&& cb, int timeout_ms = 1) {
            struct pollfd pfd {};
            pfd.fd = fd;
            pfd.events = POLLIN;

            poll(&pfd, 1, timeout_ms);

            // Drain ring buffer in bulk
            while (true) {
                auto* hdr = reinterpret_cast<tpacket_hdr*>(ring + (rx_idx * FRAME_SIZE));
                if (!(hdr->tp_status & TP_STATUS_USER)) break;

                auto* sll = reinterpret_cast<sockaddr_ll*>(reinterpret_cast<uint8_t*>(hdr) + TPACKET_ALIGN(sizeof(tpacket_hdr)));

                // Process only incoming packets, filter outgoing noise
                if (sll->sll_pkttype != PACKET_OUTGOING) {
                    std::span<uint8_t> pkt{ reinterpret_cast<uint8_t*>(hdr) + hdr->tp_mac, hdr->tp_len };
                    // Core call: since cb is template parameter, compiler inlines 100%
                    cb(pkt);
                }

                hdr->tp_status = TP_STATUS_KERNEL;
                rx_idx = (rx_idx + 1) % FRAME_NR;
            }
        }

    private:
        std::string iface;
    };
}
