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
        uint16_t flags; // °üº¬ Data Offset, Reserved ºÍ TCP Flags
        uint16_t window;
        uint16_t check;
        uint16_t urg_ptr;
    };
#pragma pack(pop)
}