#pragma once
#include <cstdint>
#include <netinet/in.h>
#include <array>
#include <atomic>
#include <span>
#include <cstddef>

namespace Scalpel::Net {
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
        uint32_t saddr;
        uint32_t daddr;
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
static_assert(Capacity > 1, "SpscRingBuffer Capacity must be greater than 1");

    // Zero-copy SPSC lock-free ring buffer (cross-core data from data plane to control plane, no mutex)
    template<typename T, size_t Capacity = 1024> // Effective usable capacity is Capacity - 1 because one slot is reserved to distinguish full from empty.
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

    // ParsedPacket provides a lightweight parsed view over an Ethernet frame.
    // It exposes Ethernet/IPv4/L4 pointers and offsets without copying packet data.
    struct ParsedPacket {
        std::span<uint8_t> raw_span;
        Net::EthernetHeader* eth = nullptr;
        Net::IPv4Header* ipv4 = nullptr;

        uint8_t l4_protocol = 0;
        size_t ihl = 0;
        size_t l4_offset = 0;
        void* l4_header = nullptr;

        Net::UDPHeader* udp() const {
            return (l4_protocol == 17) ? static_cast<Net::UDPHeader*>(l4_header) : nullptr;
        }

        Net::TCPHeader* tcp() const {
            return (l4_protocol == 6) ? static_cast<Net::TCPHeader*>(l4_header) : nullptr;
        }
        
        bool is_valid_ipv4() const { return ipv4 != nullptr && ihl >= sizeof(Net::IPv4Header); }

        static ParsedPacket parse(std::span<uint8_t> span) {
            ParsedPacket p;
            p.raw_span = span;
            
            if (span.size() < sizeof(Net::EthernetHeader)) return p;
            p.eth = reinterpret_cast<Net::EthernetHeader*>(span.data());
            if (ntohs(p.eth->proto) != 0x0800) return p; // Currently track IPv4 only
            
            size_t ip_total_len = ntohs(p.ipv4->tot_len);
            if (ip_total_len < p.ihl) {
                p.ipv4 = nullptr;
                return p;
            }
            if (span.size() < sizeof(Net::EthernetHeader) + ip_total_len) {
                return p;
            }
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
