#pragma once
#include <string_view>
#include <array>
#include <span>
#include <cstring>
#include <expected>
#include <string>
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

        // Kernel ring-buffer helpers — implementation in NetworkEngine.cpp.
        // Hides <poll.h> and <linux/if_packet.h> from all clients.
        void do_poll(int timeout_ms);
        bool peek_frame(std::span<uint8_t>& out);
        void advance_frame();

    public:
        explicit RawSocketManager(std::string_view iface_name);
        ~RawSocketManager();
        std::expected<void, std::string> init();
        int get_fd() const { return fd; }

        void poll_rx(int timeout_ms) { do_poll(timeout_ms); }
        bool peek_rx_frame(std::span<uint8_t>& out) { return peek_frame(out); }
        void finish_rx_frame() { advance_frame(); }

        // poll_and_dispatch must remain in the header — template instantiation
        // requires the full body to be visible at each call site.
        template<typename Callback>
        void poll_and_dispatch(Callback&& cb, int timeout_ms = 1) {
            do_poll(timeout_ms);
            std::span<uint8_t> pkt;
            while (peek_frame(pkt)) {
                cb(pkt);
                advance_frame();
            }
        }
    };
}
