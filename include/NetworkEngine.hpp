#pragma once
#include <string_view>
#include <array>
#include <span>
#include <cstring>
#include <expected>
#include <string>
#include <cstdint>
#include "Headers_util.hpp"

namespace HPGTP::Engine {
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

        // Avoids exposing <net/if.h> to clients
        static constexpr size_t IFACE_NAME_MAX = 16;
        std::array<char, IFACE_NAME_MAX> iface{};

        // Kernel ring-buffer helpers
        void do_poll(int timeout_ms);
        bool peek_frame(std::span<uint8_t>& out);
        void advance_frame();

    public:
        explicit RawSocketManager(std::string_view iface_name);
        ~RawSocketManager();
        std::expected<void, std::string> init();
        int get_fd() const { return fd; }

        // telemetry_flag: bit0 = do_poll path; bit1 = worker RX thread
        void notify_rx_poll_fatal(int err, std::uint8_t telemetry_flag);

        bool peek_rx_frame(std::span<uint8_t>& out) { return peek_frame(out); }
        void finish_rx_frame() { advance_frame(); }
    };
}
