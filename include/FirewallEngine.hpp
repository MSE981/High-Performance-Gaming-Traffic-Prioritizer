#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include "Headers.hpp"
#include "Config.hpp"

namespace Scalpel::Logic {

    // TCP connection state for stateful inspection.
    enum class ConnState : uint8_t {
        SYN_SENT    = 0,  // LAN sent SYN; waiting for SYN-ACK from server
        ESTABLISHED = 1,  // Three-way handshake complete (TCP) or first packet seen (UDP)
        FIN_WAIT    = 2,  // FIN seen; connection draining
    };

    // Stateful inbound default-deny firewall with TCP connection tracking.
    //
    // Table keyed on (remote_ip, remote_port, proto) so both outbound writes (Core 3)
    // and inbound reads (Core 2) hash to the same bucket cluster.
    // Full 5-tuple (+ lan_ip/lan_port) is stored to distinguish concurrent sessions
    // from different LAN devices to the same server:port.
    //
    // TCP state machine:
    //   Core 3 sees LAN SYN        → create entry: SYN_SENT
    //   Core 2 sees server SYN-ACK → transition:   SYN_SENT → ESTABLISHED  (allow)
    //   Core 2 sees server data    → if ESTABLISHED: allow + refresh tick
    //   Core 2/3 see FIN           → transition:   ESTABLISHED → FIN_WAIT
    //   Core 2/3 see RST           → deactivate entry immediately
    //
    // UDP: any outbound packet → ESTABLISHED immediately (connectionless, no handshake).
    // ICMP: always allowed in both directions (ping reply, unreachable, TTL-exceeded).
    //
    // All state lives in a fixed-size pre-allocated array — zero dynamic allocation per packet.
    class FirewallEngine {
        struct ConnTrackEntry {
            uint32_t  remote_ip   = 0;
            uint16_t  remote_port = 0;   // network byte order
            uint32_t  lan_ip      = 0;
            uint16_t  lan_port    = 0;   // network byte order
            uint8_t   protocol    = 0;
            std::atomic<ConnState> state{ConnState::SYN_SENT};
            std::atomic<bool>     active{false};
            std::atomic<uint32_t> last_tick{0};
        };

        static constexpr size_t   TABLE_SIZE          = 65536;
        static constexpr size_t   PROBE_LIMIT         = 64;   // raised to tolerate deletion gaps
        static constexpr uint32_t TIMEOUT_SYN_SENT    = 30;   // 30 s — unacknowledged SYN expires fast
        static constexpr uint32_t TIMEOUT_ESTABLISHED = 300;  // 5 min — active session
        static constexpr uint32_t TIMEOUT_FIN_WAIT    = 30;   // 30 s — closing connection drains quickly

        std::array<ConnTrackEntry, TABLE_SIZE> table{};
        std::atomic<uint32_t> current_tick{0};

        // Per-device block list: written by Core 1 watchdog, read by Core 2/3 hot path.
        // Using a small linear array — max 64 blocked devices, O(n) scan is negligible.
        static constexpr size_t MAX_BLOCKED = 64;
        std::array<Net::IPv4Net, MAX_BLOCKED> blocked_ips{};
        std::atomic<uint8_t> blocked_count{0};

        // FNV-1a keyed on remote side only — ensures outbound writes and inbound reads
        // hash to the same bucket, making cross-direction lookup possible.
        static uint32_t hash_remote(uint32_t remote_ip, uint16_t remote_port, uint8_t proto) {
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

        static uint32_t timeout_for(ConnState s) {
            if (s == ConnState::ESTABLISHED) return TIMEOUT_ESTABLISHED;
            if (s == ConnState::FIN_WAIT)    return TIMEOUT_FIN_WAIT;
            return TIMEOUT_SYN_SENT;
        }

        bool is_expired(const ConnTrackEntry& e) const {
            uint32_t tick = current_tick.load(std::memory_order_relaxed);
            uint32_t lt   = e.last_tick.load(std::memory_order_relaxed);
            ConnState st  = e.state.load(std::memory_order_relaxed);
            return (tick - lt) > timeout_for(st);
        }

    public:
        // Core 1: advance logical clock (called at 1 Hz by watchdog)
        void tick() { current_tick.fetch_add(1, std::memory_order_relaxed); }

        // Core 1: rebuild blocked IP list from Config::DEVICE_POLICY_TABLE
        void sync_blocked_ips() {
            uint8_t cnt = 0;
            for (size_t i = 0; i < Config::DEVICE_POLICY_COUNT && cnt < MAX_BLOCKED; ++i) {
                if (Config::DEVICE_POLICY_TABLE[i].blocked)
                    blocked_ips[cnt++] = Config::DEVICE_POLICY_TABLE[i].ip;
            }
            blocked_count.store(cnt, std::memory_order_release);
        }

        // Core 2/3 hot path: O(n) scan over tiny list
        bool is_blocked_ip(Net::IPv4Net ip) const {
            uint8_t cnt = blocked_count.load(std::memory_order_acquire);
            for (uint8_t i = 0; i < cnt; ++i)
                if (blocked_ips[i] == ip) return true;
            return false;
        }

        // Core 3 (LAN→WAN): register or refresh outbound session BEFORE SNAT.
        //
        // TCP SYN (no ACK)  → new entry in SYN_SENT state.
        // TCP other         → update existing entry (refresh tick, handle FIN/RST transitions);
        //                     if no entry exists (e.g. mid-session restart), create as ESTABLISHED.
        // UDP               → create or refresh ESTABLISHED entry.
        void track_outbound(const Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4()) return;
            uint8_t proto = pkt.l4_protocol;
            if (proto != 6 && proto != 17) return;

            uint32_t remote_ip = pkt.ipv4->daddr;
            uint32_t lan_ip    = pkt.ipv4->saddr;
            uint16_t remote_port, lan_port;

            if (proto == 17) {
                auto udp = pkt.udp();
                if (!udp) return;
                remote_port = udp->dest;
                lan_port    = udp->source;
            } else {
                auto tcp = pkt.tcp();
                if (!tcp) return;
                remote_port = tcp->dest;
                lan_port    = tcp->source;
            }

            uint32_t h = hash_remote(remote_ip, remote_port, proto) % TABLE_SIZE;
            int32_t  free_slot = -1;

            uint32_t tick = current_tick.load(std::memory_order_relaxed);

            for (size_t i = 0; i < PROBE_LIMIT; ++i) {
                size_t idx = (h + i) % TABLE_SIZE;
                auto& e = table[idx];

                if (!e.active.load(std::memory_order_acquire) || is_expired(e)) {
                    if (free_slot == -1) free_slot = static_cast<int32_t>(idx);
                    continue;
                }

                // Full 5-tuple match: update existing entry
                if (e.remote_ip == remote_ip && e.remote_port == remote_port &&
                    e.lan_ip    == lan_ip    && e.lan_port    == lan_port    &&
                    e.protocol  == proto) {

                    if (proto == 6) {
                        uint16_t flags = ntohs(pkt.tcp()->res1_doff_flags);
                        if (flags & 0x0004) {          // RST
                            e.active.store(false, std::memory_order_release);
                        } else if (flags & 0x0001) {   // FIN
                            e.state.store(ConnState::FIN_WAIT, std::memory_order_relaxed);
                            e.last_tick.store(tick, std::memory_order_relaxed);
                        } else {
                            e.last_tick.store(tick, std::memory_order_relaxed);
                        }
                    } else {
                        e.last_tick.store(tick, std::memory_order_relaxed);
                    }
                    return;
                }
            }

            // No existing entry — create at first free slot
            if (free_slot == -1) return;
            auto& ne      = table[free_slot];
            ne.remote_ip   = remote_ip;
            ne.remote_port = remote_port;
            ne.lan_ip      = lan_ip;
            ne.lan_port    = lan_port;
            ne.protocol    = proto;
            ne.last_tick.store(tick, std::memory_order_relaxed);

            if (proto == 6) {
                uint16_t flags = ntohs(pkt.tcp()->res1_doff_flags);
                ne.state.store(((flags & 0x0002) && !(flags & 0x0010))
                               ? ConnState::SYN_SENT
                               : ConnState::ESTABLISHED, std::memory_order_relaxed);
            } else {
                ne.state.store(ConnState::ESTABLISHED, std::memory_order_relaxed);
            }
            // Release store on active: ensures all identity fields are visible to other cores
            ne.active.store(true, std::memory_order_release);
        }

        // Core 2 (WAN→LAN): enforce stateful inspection BEFORE DNAT.
        // Returns true  = allow packet.
        // Returns false = drop packet (unsolicited inbound or handshake violation).
        //
        // TCP rules:
        //   SYN_SENT    + SYN-ACK → ESTABLISHED, allow   (completing the three-way handshake)
        //   SYN_SENT    + other   → block                 (data before handshake — suspicious)
        //   ESTABLISHED + data    → allow, refresh tick
        //   ESTABLISHED + FIN     → FIN_WAIT, allow
        //   any state   + RST     → deactivate, allow     (RST must reach LAN client)
        //   FIN_WAIT    + any     → allow briefly          (final ACKs must pass)
        //   no entry              → block
        bool check_inbound(const Net::ParsedPacket& pkt) {
            if (!pkt.is_valid_ipv4()) return false;
            uint8_t proto = pkt.l4_protocol;

            // ICMP is connectionless — allow all (ping reply, port-unreachable, TTL-exceeded)
            if (proto == 1) return true;
            if (proto != 6 && proto != 17) return false;

            uint32_t remote_ip = pkt.ipv4->saddr;
            uint16_t sport;

            if (proto == 17) {
                auto udp = pkt.udp();
                if (!udp) return false;
                sport = udp->source;
            } else {
                auto tcp = pkt.tcp();
                if (!tcp) return false;
                sport = tcp->source;
            }

            uint32_t h = hash_remote(remote_ip, sport, proto) % TABLE_SIZE;
            uint32_t tick = current_tick.load(std::memory_order_relaxed);

            for (size_t i = 0; i < PROBE_LIMIT; ++i) {
                size_t idx = (h + i) % TABLE_SIZE;
                auto& e = table[idx];

                if (!e.active.load(std::memory_order_acquire) || is_expired(e)) continue;

                if (e.remote_ip != remote_ip || e.remote_port != sport || e.protocol != proto)
                    continue;

                // Matching conntrack entry found
                if (proto == 17) {
                    e.last_tick.store(tick, std::memory_order_relaxed);
                    return true;
                }

                // TCP stateful inspection
                uint16_t flags = ntohs(pkt.tcp()->res1_doff_flags);
                bool syn = (flags & 0x0002) != 0;
                bool ack = (flags & 0x0010) != 0;
                bool fin = (flags & 0x0001) != 0;
                bool rst = (flags & 0x0004) != 0;

                if (rst) {
                    e.active.store(false, std::memory_order_release);
                    return true;
                }

                ConnState st = e.state.load(std::memory_order_relaxed);
                switch (st) {
                    case ConnState::SYN_SENT:
                        if (syn && ack) {
                            e.state.store(ConnState::ESTABLISHED, std::memory_order_relaxed);
                            e.last_tick.store(tick, std::memory_order_relaxed);
                            return true;
                        }
                        return false;

                    case ConnState::ESTABLISHED:
                        if (fin) {
                            e.state.store(ConnState::FIN_WAIT, std::memory_order_relaxed);
                        }
                        e.last_tick.store(tick, std::memory_order_relaxed);
                        return true;

                    case ConnState::FIN_WAIT:
                        e.last_tick.store(tick, std::memory_order_relaxed);
                        return true;
                }

                return false;
            }

            return false;
        }

        // Core 1 (watchdog): expire timed-out entries with state-aware timeouts (called at 1 Hz)
        void cleanup() {
            for (auto& e : table) {
                if (e.active.load(std::memory_order_relaxed) && is_expired(e)) {
                    e.active.store(false, std::memory_order_release);
                }
            }
        }
    };
}
