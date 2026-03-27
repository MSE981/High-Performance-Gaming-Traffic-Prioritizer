#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include "Headers.hpp"

namespace Scalpel::Logic {

    // Stateful inbound default-deny firewall engine.
    //
    // Design: independent conntrack table keyed on (remote_ip, remote_port, protocol).
    // Core 3 (LAN→WAN) calls track_outbound() before SNAT to register each outbound flow.
    // Core 2 (WAN→LAN) calls check_inbound()  before DNAT to allow only return traffic.
    // Core 1 (watchdog) calls tick() + cleanup() at 1 Hz to expire stale entries.
    //
    // All state is in a fixed-size pre-allocated array — zero dynamic allocation per packet.
    class FirewallEngine {
        struct ConnTrackEntry {
            uint32_t remote_ip   = 0;
            uint16_t remote_port = 0;  // network byte order, matches packet fields directly
            uint8_t  protocol    = 0;  // 6=TCP, 17=UDP
            bool     active      = false;
            uint32_t last_tick   = 0;
        };

        static constexpr size_t   TABLE_SIZE             = 65536;
        static constexpr uint32_t SESSION_TIMEOUT_TICKS  = 300;  // 300 seconds (5 minutes)
        static constexpr size_t   PROBE_LIMIT            = 32;   // linear-probe cap per lookup

        std::array<ConnTrackEntry, TABLE_SIZE> table{};
        uint32_t current_tick = 0;

        static uint32_t hash_conn(uint32_t remote_ip, uint16_t remote_port, uint8_t proto) {
            uint32_t h = 2166136261U;
            const uint8_t* p;
            p = reinterpret_cast<const uint8_t*>(&remote_ip);
            h ^= p[0]; h *= 16777619U; h ^= p[1]; h *= 16777619U;
            h ^= p[2]; h *= 16777619U; h ^= p[3]; h *= 16777619U;
            p = reinterpret_cast<const uint8_t*>(&remote_port);
            h ^= p[0]; h *= 16777619U; h ^= p[1]; h *= 16777619U;
            h ^= proto; h *= 16777619U;
            return h;
        }

    public:
        // Core 1: advance logical clock for timeout tracking
        void tick() { current_tick++; }

        // Core 3 (LAN→WAN, upstream): register outbound flow BEFORE SNAT.
        // Stores (server_ip, server_port, protocol) so inbound return traffic can be matched.
        void track_outbound(const Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4()) return;
            uint8_t proto = pkt.l4_protocol;
            if (proto != 6 && proto != 17) return;

            uint16_t dport = 0;
            if (proto == 17) {
                auto udp = pkt.udp();
                if (!udp) return;
                dport = udp->dest;   // kept in network byte order
            } else {
                auto tcp = pkt.tcp();
                if (!tcp) return;
                dport = tcp->dest;
            }

            uint32_t remote_ip = pkt.ipv4->daddr;
            uint32_t h = hash_conn(remote_ip, dport, proto) % TABLE_SIZE;

            for (size_t i = 0; i < PROBE_LIMIT; ++i) {
                size_t idx = (h + i) % TABLE_SIZE;
                auto& e = table[idx];

                // Reuse: exact match (refresh) or empty/expired slot (evict)
                if (e.active && e.remote_ip == remote_ip && e.remote_port == dport && e.protocol == proto) {
                    e.last_tick = current_tick;
                    return;
                }
                if (!e.active || (current_tick - e.last_tick > SESSION_TIMEOUT_TICKS)) {
                    e.remote_ip   = remote_ip;
                    e.remote_port = dport;
                    e.protocol    = proto;
                    e.active      = true;
                    e.last_tick   = current_tick;
                    return;
                }
            }
            // Table segment full — drop the registration (flow will be blocked inbound; acceptable)
        }

        // Core 2 (WAN→LAN, downstream): check if inbound packet is return traffic.
        // Returns true  = packet is allowed (established session or ICMP).
        // Returns false = packet should be dropped (unsolicited inbound).
        bool check_inbound(const Net::ParsedPacket& pkt) const {
            if (!pkt.is_valid_ipv4()) return false;
            uint8_t proto = pkt.l4_protocol;

            // ICMP is connectionless; allow all ICMP from WAN (ping replies, unreachable, TTL exceeded)
            if (proto == 1) return true;

            if (proto != 6 && proto != 17) return false;

            uint16_t sport = 0;
            if (proto == 17) {
                auto udp = pkt.udp();
                if (!udp) return false;
                sport = udp->source;
            } else {
                auto tcp = pkt.tcp();
                if (!tcp) return false;
                sport = tcp->source;
            }

            uint32_t remote_ip = pkt.ipv4->saddr;
            uint32_t h = hash_conn(remote_ip, sport, proto) % TABLE_SIZE;

            for (size_t i = 0; i < PROBE_LIMIT; ++i) {
                size_t idx = (h + i) % TABLE_SIZE;
                const auto& e = table[idx];
                if (!e.active) continue;
                if (e.remote_ip == remote_ip && e.remote_port == sport && e.protocol == proto) {
                    return true;  // established session — allow
                }
            }
            return false;  // no matching session — block
        }

        // Core 1 (watchdog): sweep table and deactivate timed-out entries (called at 1 Hz)
        void cleanup() {
            for (auto& e : table) {
                if (e.active && (current_tick - e.last_tick > SESSION_TIMEOUT_TICKS)) {
                    e.active = false;
                }
            }
        }
    };
}
