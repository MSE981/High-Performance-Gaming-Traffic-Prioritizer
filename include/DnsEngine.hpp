#pragma once
#include <span>
#include <chrono>
#include <array>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <netinet/in.h>
#include <sys/socket.h>
#include <print>
#include "Headers.hpp"
#include "Telemetry.hpp"
#include "Config.hpp"

namespace Scalpel::Logic {
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

    // Core separated DNS resolution engine
    class DnsEngine {
        // Static table: compliance with principle 3.1 - Zero Dynamic Allocation
        struct DnsCacheEntry {
            uint32_t     domain_hash  = 0;
            Net::IPv4Net ipv4_address{};
            uint32_t     expire_tick  = 0;
            std::atomic<bool> valid{false};
        };

        static constexpr size_t CACHE_SIZE = 4096;
        std::array<DnsCacheEntry, CACHE_SIZE> cache{};
        Net::SpscRingBuffer<DnsMessage, 1024> response_queue{};

        std::atomic<uint32_t> current_tick{0};

        // Upstream DNS server IPs (NBO); default-constructed = 0.0.0.0 = not configured
        std::atomic<Net::IPv4Net> upstream_primary_ip{};
        std::atomic<Net::IPv4Net> upstream_secondary_ip{};
        std::atomic<bool>     redirect_enabled{false};

        // Static DNS records: written by Core 1 watchdog, read by Core 3 hot path
        struct StaticRecord {
            uint32_t     domain_hash = 0;
            Net::IPv4Net ip{};
        };
        static constexpr size_t MAX_STATIC = 64;
        std::array<StaticRecord, MAX_STATIC> static_records{};
        std::atomic<uint8_t> static_count{0};  // release-stored last after all entries written

        // FNV-1a Hash for domain string (QNAME)
        static uint32_t hash_qname(const uint8_t* qname, size_t max_len) {
            uint32_t h = 2166136261U;
            size_t i = 0;
            while (i < max_len && qname[i] != 0) { // DNS labels end with 0
                h ^= qname[i]; h *= 16777619U;
                i++;
            }
            return h;
        }

        // Swap Eth/IP/UDP headers and inject a single A-record answer, then send back on bounce_fd.
        void do_bounce(Net::ParsedPacket& pkt, DnsHeader* dns, Net::UDPHeader* udp,
                       Net::IPv4Net ip, int bounce_fd) {
            size_t old_len = pkt.raw_span.size();
            size_t new_len = old_len + 16;
            if (new_len > 1500) return; // MTU guard

            dns->flags   = htons(ntohs(dns->flags) | 0x8180); // QR=1, AA=1, RA=1
            dns->ancount = htons(1);

            uint8_t* tail = pkt.raw_span.data() + old_len;
            tail[0] = 0xC0; tail[1] = 0x0C;                   // Name pointer to question
            tail[2] = 0x00; tail[3] = 0x01;                   // Type A
            tail[4] = 0x00; tail[5] = 0x01;                   // Class IN
            tail[6] = 0x00; tail[7] = 0x00; tail[8] = 0x01; tail[9] = 0x2C; // TTL 300
            tail[10] = 0x00; tail[11] = 0x04;                 // RDLength = 4
            uint32_t raw_ip = ip.raw();
            std::memcpy(&tail[12], &raw_ip, 4);

            for (int i = 0; i < 6; ++i) std::swap(pkt.eth->src[i], pkt.eth->dest[i]);
            Net::IPv4Net s_ip = pkt.ipv4->saddr;
            pkt.ipv4->saddr = pkt.ipv4->daddr;
            pkt.ipv4->daddr = s_ip;
            pkt.ipv4->tot_len = htons(static_cast<uint16_t>(new_len - sizeof(Net::EthernetHeader)));
            pkt.ipv4->check = 0;
            uint32_t sum = 0;
            const uint16_t* ip_words = reinterpret_cast<const uint16_t*>(pkt.ipv4);
            for (size_t i = 0; i < pkt.ihl / 2; ++i) sum += ntohs(ip_words[i]);
            sum = (sum >> 16) + (sum & 0xFFFF);
            sum += (sum >> 16);
            pkt.ipv4->check = htons(~static_cast<uint16_t>(sum));

            uint16_t s_port = udp->source;
            udp->source = udp->dest;
            udp->dest   = s_port;
            udp->len    = htons(static_cast<uint16_t>(new_len - sizeof(Net::EthernetHeader) - pkt.ihl));
            udp->check  = 0;

            if (send(bounce_fd, pkt.raw_span.data(), new_len, MSG_DONTWAIT) < 0)
                Telemetry::instance().core_metrics[3].dropped[1].fetch_add(1, std::memory_order_relaxed);
        }

        // Rewrite IPv4 destination + recompute IP header checksum (used for DNS redirect)
        static void rewrite_upstream(Net::ParsedPacket& pkt, Net::IPv4Net upstream_ip) {
            pkt.ipv4->daddr = upstream_ip;
            pkt.ipv4->check = 0;
            uint32_t sum = 0;
            const uint16_t* words = reinterpret_cast<const uint16_t*>(pkt.ipv4);
            for (size_t i = 0; i < pkt.ihl / 2; ++i) sum += ntohs(words[i]);
            sum = (sum >> 16) + (sum & 0xFFFF);
            sum += (sum >> 16);
            pkt.ipv4->check = htons(~static_cast<uint16_t>(sum));
        }

    public:
        void tick() { current_tick.fetch_add(1, std::memory_order_relaxed); }

        // Core 1: apply upstream DNS server config (called from watchdog on dns_config_dirty)
        void set_upstream(Net::IPv4Net primary, Net::IPv4Net secondary) {
            upstream_primary_ip.store(primary,   std::memory_order_release);
            upstream_secondary_ip.store(secondary, std::memory_order_release);
        }

        // Core 1: enable/disable DNS redirect
        void set_redirect(bool enabled) {
            redirect_enabled.store(enabled, std::memory_order_relaxed);
        }

        // Core 1: rebuild static record table from Config::STATIC_DNS_TABLE
        void reload_static_records() {
            uint8_t cnt = 0;
            for (size_t i = 0; i < Config::STATIC_DNS_COUNT && cnt < MAX_STATIC; ++i) {
                static_records[cnt].domain_hash = Config::STATIC_DNS_TABLE[i].domain_hash;
                static_records[cnt].ip          = Config::STATIC_DNS_TABLE[i].ip;
                ++cnt;
            }
            // Release store: Core 3 acquire-loads static_count before scanning static_records[]
            static_count.store(cnt, std::memory_order_release);
        }

        // Core 3 (Upstream: LAN -> WAN) - three-stage lookup: static → cache → redirect
        bool process_query(Net::ParsedPacket& pkt, int bounce_fd) {
            if (!pkt.is_valid_ipv4() || pkt.l4_protocol != 17) return false;

            auto udp = pkt.udp();
            if (!udp || ntohs(udp->dest) != 53) return false;

            size_t dns_offset = pkt.l4_offset + sizeof(Net::UDPHeader);
            if (pkt.raw_span.size() < dns_offset + sizeof(DnsHeader) + 1) return false;

            auto dns = reinterpret_cast<DnsHeader*>(pkt.raw_span.data() + dns_offset);
            if ((ntohs(dns->flags) & 0x8000) != 0) return false; // drop responses
            if (ntohs(dns->qdcount) != 1) return false;

            size_t qname_offset = dns_offset + sizeof(DnsHeader);
            if (qname_offset >= pkt.raw_span.size()) return false;

            uint32_t h = hash_qname(pkt.raw_span.data() + qname_offset,
                                    pkt.raw_span.size() - qname_offset);

            // Stage 1: static records (highest priority, no TTL)
            uint8_t scnt = static_count.load(std::memory_order_acquire);
            for (uint8_t i = 0; i < scnt; ++i) {
                if (static_records[i].domain_hash == h) {
                    do_bounce(pkt, dns, udp, static_records[i].ip, bounce_fd);
                    return true;
                }
            }

            // Stage 2: DNS cache lookup
            size_t idx = h % CACHE_SIZE;
            if (cache[idx].valid.load(std::memory_order_acquire)) {
                if (cache[idx].domain_hash == h &&
                    current_tick.load(std::memory_order_relaxed) <= cache[idx].expire_tick) {
                    do_bounce(pkt, dns, udp, cache[idx].ipv4_address, bounce_fd);
                    return true;
                }
                cache[idx].valid.store(false, std::memory_order_relaxed); // invalidate stale
            }

            // Stage 3: cache miss — redirect query to configured upstream DNS server
            if (redirect_enabled.load(std::memory_order_relaxed)) {
                Net::IPv4Net upstream = upstream_primary_ip.load(std::memory_order_relaxed);
                if (upstream.raw() != 0) rewrite_upstream(pkt, upstream);
            }
            return false; // continue pipeline → NAT handles SNAT to upstream
        }

        // Core 2 (Downstream: WAN -> LAN) - intercept external responses, queue to control plane
        void intercept_response(const Net::ParsedPacket& pkt) {
            if (pkt.raw_span.size() > 512 || !pkt.is_valid_ipv4()) return;
            if (pkt.l4_protocol != 17) return;

            auto udp = pkt.udp();
            if (!udp || ntohs(udp->source) != 53) return; // Only listen for external DNS

            DnsMessage msg;
            msg.len = pkt.raw_span.size();
            std::memcpy(msg.data.data(), pkt.raw_span.data(), pkt.raw_span.size());
            // Queue to lock-free queue (Core 1 watchdog processes later), drops on overflow
            response_queue.push(msg); 
        }

        // Control plane (Core 1) background learning loop (decoupled from high-frequency data)
        void process_background_tasks() {
            DnsMessage msg;
            int counter = 0;
            while (response_queue.pop(msg) && counter++ < 32) { // Max 32 packets/sec, don't starve watchdog
                auto ip = reinterpret_cast<const Net::IPv4Header*>(msg.data.data() + sizeof(Net::EthernetHeader));
                size_t ihl = (ip->ver_ihl & 0x0F) * 4;
                size_t dns_offset = sizeof(Net::EthernetHeader) + ihl + sizeof(Net::UDPHeader);
                
                auto dns = reinterpret_cast<const DnsHeader*>(msg.data.data() + dns_offset);
                if (ntohs(dns->ancount) == 0) continue;

                size_t qname_offset = dns_offset + sizeof(DnsHeader);
                if (qname_offset >= msg.len) continue;
                
                uint32_t h = hash_qname(msg.data.data() + qname_offset, msg.len - qname_offset);

                // Packet parsing is complex with uncertain pointers: justifies separation to Core 1
                size_t ptr = qname_offset;
                while (ptr < msg.len && msg.data[ptr] != 0) { ptr += msg.data[ptr] + 1; }
                ptr += 5; // Skip null byte (1) + qtype (2) + qclass (2)

                // First answer record
                if (ptr + 12 <= msg.len) {
                    // Type A (0x0001) record
                    if (msg.data[ptr+2] == 0x00 && msg.data[ptr+3] == 0x01) {
                        Net::IPv4Net ipv4{*reinterpret_cast<const uint32_t*>(&msg.data[ptr+10])};

                        size_t idx = h % CACHE_SIZE;
                        cache[idx].domain_hash  = h;
                        cache[idx].ipv4_address = ipv4;
                        cache[idx].expire_tick = current_tick.load(std::memory_order_relaxed) + 300;
                        // Principle 3.0: release barrier ensures memory writes complete before data plane acquires true
                        cache[idx].valid.store(true, std::memory_order_release);

                        // std::println("[Control Plane] Learned DNS record hash:{}", h);
                    }
                }
            }
        }
    };
}


