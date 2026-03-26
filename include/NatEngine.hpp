#pragma once
#include <span>
#include <chrono>
#include <array>
#include <cstdint>
#include <netinet/in.h>
#include "Headers.hpp"
#include "Processor.hpp"

namespace Scalpel::Logic {
    // Incremental checksum update (RFC 1624): HC' = ~(~HC + ~m + m')
    // Essential algorithm for ultra-fast IP/port conversion in user space, avoids full recomputation overhead
    static inline void update_checksum_16(uint16_t& check, uint16_t old_val, uint16_t new_val) {
        uint32_t sum = (~ntohs(check) & 0xFFFF) + (~ntohs(old_val) & 0xFFFF) + ntohs(new_val);
        sum = (sum & 0xFFFF) + (sum >> 16);
        check = htons(~(sum + (sum >> 16)));
    }
    
    static inline void update_checksum_32(uint16_t& check, uint32_t old_val, uint32_t new_val) {
        update_checksum_16(check, old_val & 0xFFFF, new_val & 0xFFFF);
        update_checksum_16(check, old_val >> 16, new_val >> 16);
    }

    // True zero-copy user-space NAT engine
    class NatEngine {
        struct NatSession {
            FlowKey internal_key; // saddr=LAN_IP, sport=LAN_Port, daddr=WAN_DEST, dport=WAN_DEST_PORT
            uint16_t external_port;
            uint32_t last_active_tick;
            bool active = false;
        };

        struct UpnpMapping {
            uint32_t internal_ip = 0;
            uint16_t internal_port = 0;
            uint16_t external_port = 0;
            uint8_t protocol = 0;
            alignas(64) std::atomic<bool> active{false};
        };

        // Principle 3.1: Zero dynamic allocation. All table entries fixed at construction, no std::unordered_map
        static constexpr size_t MAX_SESSIONS = 65536;

        // Forward hash table: LAN -> WAN (mask private IP on outbound lookup)
        std::array<NatSession, MAX_SESSIONS> sessions{};
        // Reverse mapping: external port -> session index (O(1) ultra-fast inbound lookup via direct array indexing!)
        std::array<int32_t, 65536> port_to_index{};

        std::array<UpnpMapping, 256> upnp_rules{};
        alignas(64) std::atomic<size_t> upnp_cursor{0};

        uint16_t port_cursor = 10000;
        uint32_t wan_ip = 0;
        uint32_t current_tick = 0;

        uint32_t hash_flow(const FlowKey& k) const {
            uint32_t h = 2166136261U;
            auto proc = [&](const auto& val) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
                for(size_t i=0; i<sizeof(val); ++i) { h ^= p[i]; h *= 16777619U; }
            };
            proc(k.saddr); proc(k.daddr); proc(k.sport); proc(k.dport);
            return h;
        }

    public:
        explicit NatEngine() {
            port_to_index.fill(-1);
        }

        void set_wan_ip(uint32_t ip) { wan_ip = ip; }
        
        // Core 1 (control plane): add or override UPnP rule
        void add_upnp_rule(uint16_t ext_port, uint32_t int_ip, uint16_t int_port, uint8_t proto) {
            uint16_t net_ext_port = htons(ext_port);
            uint16_t net_int_port = htons(int_port);

            for (auto& rule : upnp_rules) {
                if (rule.active.load(std::memory_order_relaxed)) {
                    if (rule.external_port == net_ext_port && rule.protocol == proto) {
                        rule.internal_ip = int_ip;
                        rule.internal_port = net_int_port;
                        return; 
                    }
                }
            }
            size_t idx = upnp_cursor.fetch_add(1, std::memory_order_relaxed) % upnp_rules.size();
            upnp_rules[idx].internal_ip = int_ip;
            upnp_rules[idx].internal_port = net_int_port;
            upnp_rules[idx].external_port = net_ext_port;
            upnp_rules[idx].protocol = proto;
            upnp_rules[idx].active.store(true, std::memory_order_release);
        }

        // Low-frequency tick driver (interfaces watchdog, decouples high-frequency syscalls)
        void tick() { current_tick++; }

        // Outbound (WAN_TX): LAN -> WAN, replace source IP/port (SNAT)
        bool process_outbound(Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4()) return false;
            if (wan_ip == 0) return false;
            
            auto ip = pkt.ipv4;
            if (ip->protocol != 6 && ip->protocol != 17) return false;

            uint16_t* sport_ptr = nullptr;
            uint16_t* check_ptr = nullptr;
            uint16_t dport = 0;

            if (ip->protocol == 17) { 
                auto udp = pkt.udp();
                if (!udp) return false;
                sport_ptr = &udp->source; dport = udp->dest; check_ptr = &udp->check;
            } else { 
                auto tcp = pkt.tcp();
                if (!tcp) return false;
                sport_ptr = &tcp->source; dport = tcp->dest; check_ptr = &tcp->check;
            }

            // UPnP fast-path outbound: skip entirely when no rules have ever been added
            if (upnp_cursor.load(std::memory_order_relaxed) > 0) for (auto& rule : upnp_rules) {
                if (rule.active.load(std::memory_order_acquire)) {
                    if (rule.protocol == ip->protocol && rule.internal_ip == ip->saddr && rule.internal_port == *sport_ptr) {
                        // Update IP checksum for saddr change
                        update_checksum_32(ip->check, ip->saddr, wan_ip);
                        // Update transport checksum for saddr change (if applicable)
                        if (check_ptr && *check_ptr != 0) {
                            update_checksum_32(*check_ptr, ip->saddr, wan_ip);
                        }

                        ip->saddr = wan_ip;
                        *sport_ptr = rule.external_port;
                        return true;
                    }
                }
            }

            // Standard SNAT Processing
            FlowKey key{ip->saddr, ip->daddr, *sport_ptr, dport};
            uint32_t h = hash_flow(key) % MAX_SESSIONS;
            
            uint16_t ext_port = 0;

            for (size_t i = 0; i < 32; ++i) { // Cap linear probe to 32 slots to avoid livelock
                size_t idx = (h + i) % MAX_SESSIONS;
                if (!sessions[idx].active || (current_tick - sessions[idx].last_active_tick > 300)) {
                    // Evict stale session, reclaim port
                    if (sessions[idx].active) port_to_index[ntohs(sessions[idx].external_port)] = -1;
                    
                    sessions[idx].internal_key = key;
                    sessions[idx].external_port = htons(port_cursor++);
                    if (port_cursor > 60000) port_cursor = 10000;
                    sessions[idx].active = true;
                    sessions[idx].last_active_tick = current_tick;
                    ext_port = sessions[idx].external_port;
                    port_to_index[ntohs(ext_port)] = idx;
                    break;
                }
                if (sessions[idx].internal_key == key) {
                    sessions[idx].last_active_tick = current_tick;
                    ext_port = sessions[idx].external_port;
                    break;
                }
            }

            if (!ext_port) return false; // NAT TABLE FULL

            // Zero-copy O(1) in-place rewrite on the memory span
            update_checksum_32(ip->check, ip->saddr, wan_ip);
            if (check_ptr && *check_ptr != 0) {
                update_checksum_32(*check_ptr, ip->saddr, wan_ip);
                update_checksum_16(*check_ptr, *sport_ptr, ext_port);
            }

            ip->saddr = wan_ip;
            *sport_ptr = ext_port;

            return true;
        }

        // Inbound (WAN_RX): WAN -> LAN, replace destination IP/port (DNAT)
        bool process_inbound(Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4()) return false;
            auto ip = pkt.ipv4;

            if (ip->protocol != 6 && ip->protocol != 17) return false;
            if (ip->daddr != wan_ip) return false; // Only translate packets destined for the WAN IP

            uint16_t* dport_ptr = nullptr;
            uint16_t* check_ptr = nullptr;
            uint16_t sport = 0;

            if (ip->protocol == 17) {
                auto udp = pkt.udp();
                if (!udp) return false;
                dport_ptr = &udp->dest; sport = udp->source; check_ptr = &udp->check;
            } else {
                auto tcp = pkt.tcp();
                if (!tcp) return false;
                dport_ptr = &tcp->dest; sport = tcp->source; check_ptr = &tcp->check;
            }

            // UPnP fast-path inbound: skip entirely when no rules have ever been added
            if (upnp_cursor.load(std::memory_order_relaxed) > 0) for (auto& rule : upnp_rules) {
                if (rule.active.load(std::memory_order_acquire)) {
                    if (rule.protocol == ip->protocol && rule.external_port == *dport_ptr) {
                        // Update IP checksum for daddr change
                        update_checksum_32(ip->check, ip->daddr, rule.internal_ip);
                        // Update transport checksum for daddr change (if applicable)
                        if (check_ptr && *check_ptr != 0) {
                            update_checksum_32(*check_ptr, ip->daddr, rule.internal_ip);
                        }

                        ip->daddr = rule.internal_ip;
                        *dport_ptr = rule.internal_port;
                        return true;
                    }
                }
            }

            // Standard DNAT Processing
            // O(1) direct-index reverse lookup
            int32_t idx = port_to_index[ntohs(*dport_ptr)];
            if (idx == -1 || !sessions[idx].active || 
                sessions[idx].internal_key.daddr != ip->saddr || 
                sessions[idx].internal_key.dport != sport) {
                return false; 
            }

            sessions[idx].last_active_tick = current_tick;
            uint32_t internal_ip = sessions[idx].internal_key.saddr;
            uint16_t internal_port = sessions[idx].internal_key.sport;

            // O(1) rewrite and checksum update
            update_checksum_32(ip->check, ip->daddr, internal_ip);
            if (check_ptr && *check_ptr != 0) {
                update_checksum_32(*check_ptr, ip->daddr, internal_ip);
                update_checksum_16(*check_ptr, *dport_ptr, internal_port);
            }

            ip->daddr = internal_ip;
            *dport_ptr = internal_port;

            return true;
        }
    };
}

