#pragma once
// HPGTP = High-Performance Gaming Traffic Prioritizer. This header defines HPGTP::Net wire types.
#include <cstdint>
#include <atomic>
#include <array>
#include <span>
#include "NetworkTypes.hpp"

namespace HPGTP::Net {

    inline constexpr uint16_t eth_proto_wire_to_host(uint16_t be) noexcept {
        return static_cast<uint16_t>((be << 8) | (be >> 8));
    }

    enum class Priority : uint8_t {
        Critical = 0, // DNS, TCP-ACK
        High = 1, // Gaming
        Normal = 2  // Download, Video
    };

#pragma pack(push, 1)
    struct EthernetHeader {
        uint8_t  dest[6];
        uint8_t  src[6];
        uint16_t proto; // Network Byte Order
    };

    struct IPv4Header {
        uint8_t  ver_ihl;
        uint8_t  tos;
        uint16_t tot_len;
        uint16_t id;
        uint16_t frag_off;
        uint8_t  ttl;
        uint8_t  protocol;
        uint16_t check;
        IPv4Net  saddr;   // always NBO — from wire
        IPv4Net  daddr;   // always NBO — from wire
    };

    struct UDPHeader {
        uint16_t source;
        uint16_t dest;
        uint16_t len;
        uint16_t check;
    };

    struct TCPHeader {
        uint16_t source;
        uint16_t dest;
        uint32_t seq;
        uint32_t ack_seq;
        uint16_t res1_doff_flags;
        uint16_t window;
        uint16_t check;
        uint16_t urg_ptr;
    };
#pragma pack(pop)

    // Zero-copy SPSC lock-free ring buffer (cross-core data from data plane to control plane, no mutex)
    template<typename T, size_t Capacity = 1024>
    class SpscRingBuffer {
        std::array<T, Capacity> buffer{};
        alignas(64) std::atomic<size_t> head{0};
        alignas(64) std::atomic<size_t> tail{0};
    public:
        // Data plane call: push
        bool push(const T& item) {
            size_t current_tail = tail.load(std::memory_order_relaxed);
            size_t next_tail = (current_tail + 1) % Capacity;
            if (next_tail == head.load(std::memory_order_acquire)) return false; // Full, discard
            buffer[current_tail] = item;
            tail.store(next_tail, std::memory_order_release);
            return true;
        }

        // Control plane call: pop
        bool pop(T& item) {
            size_t current_head = head.load(std::memory_order_relaxed);
            if (current_head == tail.load(std::memory_order_acquire)) return false; // Empty
            item = buffer[current_head]; 
            head.store((current_head + 1) % Capacity, std::memory_order_release);
            return true;
        }
    };

    // Phase 2.6: Unified zero-copy packet context parser
    // Eliminate redundant scalar offset calculations in downstream modules (NAT, DNS, QoS, HeuristicProcessor)
    struct ParsedPacket {
        std::span<uint8_t> raw_span;
        Net::EthernetHeader* eth = nullptr;
        Net::IPv4Header* ipv4 = nullptr;

        uint8_t l4_protocol = 0;
        size_t ihl = 0;
        size_t l4_offset = 0;
        void* l4_header = nullptr;

        Net::UDPHeader* udp() const { return (l4_protocol == 17) ? reinterpret_cast<Net::UDPHeader*>(l4_header) : nullptr; }
        Net::TCPHeader* tcp() const { return (l4_protocol == 6) ? reinterpret_cast<Net::TCPHeader*>(l4_header) : nullptr; }
        
        bool is_valid_ipv4() const { return ipv4 != nullptr; }

        static ParsedPacket parse(std::span<uint8_t> span) {
            ParsedPacket p;
            p.raw_span = span;
            
            if (span.size() < sizeof(Net::EthernetHeader)) return p;
            p.eth = reinterpret_cast<Net::EthernetHeader*>(span.data());
            if (eth_proto_wire_to_host(p.eth->proto) != 0x0800) return p; // IPv4 only
            
            if (span.size() < sizeof(Net::EthernetHeader) + sizeof(Net::IPv4Header)) return p;
            p.ipv4 = reinterpret_cast<Net::IPv4Header*>(span.data() + sizeof(Net::EthernetHeader));
            
            p.ihl = (p.ipv4->ver_ihl & 0x0F) * 4;
            p.l4_offset = sizeof(Net::EthernetHeader) + p.ihl;
            if (span.size() < p.l4_offset) return p; // Bad packet
            
            p.l4_protocol = p.ipv4->protocol;
            if (p.l4_protocol == 17 && span.size() >= p.l4_offset + sizeof(Net::UDPHeader)) {
                p.l4_header = span.data() + p.l4_offset;
            } else if (p.l4_protocol == 6 && span.size() >= p.l4_offset + sizeof(Net::TCPHeader)) {
                p.l4_header = span.data() + p.l4_offset;
            }

            return p;
        }
    };
}
