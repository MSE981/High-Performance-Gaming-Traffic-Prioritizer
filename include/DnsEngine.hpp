#pragma once
#include <span>
#include <array>
#include <cstdint>
#include <atomic>
#include "Headers.hpp"
#include "Telemetry.hpp"
#include "Config.hpp"

namespace HPGTP::Logic {

    // Result of attempting to handle an incoming DNS query on the LAN→WAN path.
    enum class DnsQueryDisposition : std::uint8_t {
        NotHandled,      // Not a handled DNS query; frame unchanged by this engine.
        Replied,         // Response built and sent successfully on bounce_fd.
        ReplySendFailed, // Response built but egress send did not complete (frame mutated).
        Redirected,      // Upstream redirect: destination IPv4 rewritten for forwarding.
    };

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
            std::atomic<uint32_t>     domain_hash{0};
            std::atomic<Net::IPv4Net> ipv4_address{};
            std::atomic<uint32_t>     expire_tick{0};
            std::atomic<bool>         valid{false};
        };

        // Redirect flow state: core 3 records the client's original DNS server IP
        // before rewriting daddr to upstream; core 2 looks it up after NAT DNAT
        // has restored (client_ip, client_sport) on the reply, to rewrite saddr
        // back so the resolver accepts the response.
        struct alignas(64) DnsRedirectEntry {
            std::atomic<uint64_t>     key{0};
            std::atomic<Net::IPv4Net> original_dst{};
            std::atomic<uint32_t>     expire_tick{0};
        };

        static constexpr size_t CACHE_SIZE       = 4096;
        static constexpr size_t REDIRECT_SIZE    = 1024;
        static constexpr uint32_t REDIRECT_TTL   = 10;
        std::array<DnsCacheEntry, CACHE_SIZE>       cache{};
        std::array<DnsRedirectEntry, REDIRECT_SIZE> redirect_table{};
        Net::SpscRingBuffer<DnsMessage, 1024> response_queue{};

        std::atomic<uint32_t> current_tick{0};

        std::atomic<Net::IPv4Net> upstream_primary_ip{};
        std::atomic<Net::IPv4Net> upstream_secondary_ip{};
        std::atomic<Net::IPv4Net> gateway_ip{};
        std::atomic<bool>         redirect_enabled{false};

        struct StaticRecord {
            uint32_t     domain_hash = 0;
            Net::IPv4Net ip{};
        };
        static constexpr size_t MAX_STATIC = 64;
        std::array<StaticRecord, MAX_STATIC> static_records{};
        std::atomic<uint8_t> static_count{0};

        // Preconditions: parsed IPv4/UDP/DNS; `pkt.raw_span` must hold at least the current
        // Ethernet+IP+UDP+DNS wire length plus 16 bytes for the appended A record RDATA tail,
        // and the resulting frame length must not exceed 1500 bytes (see implementation checks).
        [[nodiscard]] bool do_bounce(Net::ParsedPacket& pkt, DnsHeader* dns, Net::UDPHeader* udp,
                                     Net::IPv4Net ip, int bounce_fd) noexcept;
        static void rewrite_upstream(Net::ParsedPacket& pkt, Net::UDPHeader* udp,
                                     Net::IPv4Net upstream_ip);
        void record_redirect(Net::IPv4Net client_ip, uint16_t sport_nbo,
                             Net::IPv4Net original_dst) noexcept;
        [[nodiscard]] bool lookup_redirect(Net::IPv4Net client_ip, uint16_t dport_nbo,
                                           Net::IPv4Net& out_original_dst) noexcept;

    public:
        void tick();
        void set_upstream(DnsUpstreamConfig cfg);
        void set_redirect(bool enabled);
        void set_gateway_ip(Net::IPv4Net ip) noexcept;
        void reload_static_records();
        [[nodiscard]] DnsQueryDisposition process_query(Net::ParsedPacket& pkt,
                                                        int bounce_fd) noexcept;
        void process_response(Net::ParsedPacket& pkt) noexcept;
        void process_background_tasks();
    };
}
