#pragma once
#include <cstdint>
#include <netinet/in.h>

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

    // Phase 2.6: 统一零拷贝包上下文解析器
    // 消除下游模块 (NAT, DNS, QoS, HeuristicProcessor) 冗余重叠的标量偏移计算
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
            if (ntohs(p.eth->proto) != 0x0800) return p; // 目前只追踪 IPv4 封包
            
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
