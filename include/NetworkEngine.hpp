#pragma once
#include <string_view>
#include <array>
#include <span>
#include <cstring>
#include <expected>
#include <string>
#include <poll.h>
#include <linux/if_packet.h>
#include "Headers.hpp"

namespace Scalpel::Engine {
    class RawSocketManager {
        RawSocketManager(const RawSocketManager&) = delete;
        RawSocketManager& operator=(const RawSocketManager&) = delete;

        int fd = -1;
        uint8_t* ring = nullptr;
        size_t ring_size = 0;
        uint32_t rx_idx = 0;

        static constexpr uint32_t BLOCK_SIZE = 4096 * 16;
        static constexpr uint32_t FRAME_SIZE = 2048;
        static constexpr uint32_t BLOCK_NR   = 1024;
        static constexpr uint32_t FRAME_NR   = (BLOCK_SIZE * BLOCK_NR) / FRAME_SIZE;

        // Avoids exposing <net/if.h> (IFNAMSIZ) to clients
        static constexpr size_t IFACE_NAME_MAX = 16;
        std::array<char, IFACE_NAME_MAX> iface{};

    public:
        explicit RawSocketManager(std::string_view iface_name);
        ~RawSocketManager();
        std::expected<void, std::string> init();
        int get_fd() const { return fd; }

        // poll_and_dispatch must remain in the header — template instantiation
        // requires the full body to be visible at each call site.
        template<typename Callback>
        void poll_and_dispatch(Callback&& cb, int timeout_ms = 1) {
            struct pollfd pfd{};
            pfd.fd     = fd;
            pfd.events = POLLIN;
            poll(&pfd, 1, timeout_ms);

            while (true) {
                auto* hdr = reinterpret_cast<tpacket_hdr*>(ring + (rx_idx * FRAME_SIZE));
                if (!(hdr->tp_status & TP_STATUS_USER)) break;

                auto* sll = reinterpret_cast<sockaddr_ll*>(
                    reinterpret_cast<uint8_t*>(hdr) + TPACKET_ALIGN(sizeof(tpacket_hdr)));

                if (sll->sll_pkttype != PACKET_OUTGOING) {
                    std::span<uint8_t> pkt{
                        reinterpret_cast<uint8_t*>(hdr) + hdr->tp_mac, hdr->tp_len };
                    cb(pkt);
                }

                hdr->tp_status = TP_STATUS_KERNEL;
                rx_idx = (rx_idx + 1) % FRAME_NR;
            }
        }
    };
}
