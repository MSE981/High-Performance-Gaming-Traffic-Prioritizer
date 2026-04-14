#pragma once
#include <span>
#include <array>
#include <cstdint>
#include <atomic>
#include "Headers.hpp"
#include "Telemetry.hpp"
#include "Config.hpp"

namespace HPGTP::Logic {
    struct DnsHeader {
        uint16_t id;
        uint16_t flags;
        uint16_t qdcount;
        uint16_t ancount;
        uint16_t nscount;
        uint16_t arcount;
    };

    struct DnsMessage {
        size_t len;
        std::array<uint8_t, 512> data;
    };

    struct DnsUpstreamConfig {
        Net::IPv4Net primary{};
        Net::IPv4Net secondary{};
    };

    class DnsEngine {
        struct alignas(64) DnsCacheEntry {
            uint32_t     domain_hash  = 0;
            Net::IPv4Net ipv4_address{};
            uint32_t     expire_tick  = 0;
            std::atomic<bool> valid{false};
        };

        static constexpr size_t CACHE_SIZE = 4096;
        std::array<DnsCacheEntry, CACHE_SIZE> cache{};
        Net::SpscRingBuffer<DnsMessage, 1024> response_queue{};

        std::atomic<uint32_t> current_tick{0};

        std::atomic<Net::IPv4Net> upstream_primary_ip{};
        std::atomic<Net::IPv4Net> upstream_secondary_ip{};
        std::atomic<bool>         redirect_enabled{false};

        struct StaticRecord {
            uint32_t     domain_hash = 0;
            Net::IPv4Net ip{};
        };
        static constexpr size_t MAX_STATIC = 64;
        std::array<StaticRecord, MAX_STATIC> static_records{};
        std::atomic<uint8_t> static_count{0};

        void do_bounce(Net::ParsedPacket& pkt, DnsHeader* dns, Net::UDPHeader* udp,
                       Net::IPv4Net ip, int bounce_fd);
        static void rewrite_upstream(Net::ParsedPacket& pkt, Net::IPv4Net upstream_ip);

    public:
        void tick();
        void set_upstream(DnsUpstreamConfig cfg);
        void set_redirect(bool enabled);
        void reload_static_records();
        bool process_query(Net::ParsedPacket& pkt, int bounce_fd);
        void intercept_response(const Net::ParsedPacket& pkt);
        void process_background_tasks();
    };
}
