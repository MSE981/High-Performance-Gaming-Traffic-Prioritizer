#include "FirewallEngine.hpp"
#include <mutex>
#include <netinet/in.h>

namespace HPGTP::Logic {

uint32_t FirewallEngine::hash_remote(uint32_t remote_ip, uint16_t remote_port, uint8_t proto) {
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

uint32_t FirewallEngine::timeout_for(ConnState s) {
    if (s == ConnState::ESTABLISHED) return TIMEOUT_ESTABLISHED;
    if (s == ConnState::FIN_WAIT)    return TIMEOUT_FIN_WAIT;
    return TIMEOUT_SYN_SENT;
}

bool FirewallEngine::is_expired(const ConnTrackEntry& e) const {
    uint32_t tick = current_tick.load(std::memory_order_relaxed);
    uint32_t lt   = e.last_tick.load(std::memory_order_relaxed);
    ConnState st  = e.state.load(std::memory_order_relaxed);
    return (tick - lt) > timeout_for(st);
}

void FirewallEngine::tick() { current_tick.fetch_add(1, std::memory_order_relaxed); }

void FirewallEngine::sync_blocked_ips_locked() {
    uint8_t cnt = 0;
    for (size_t i = 0; i < Config::DEVICE_POLICY_COUNT && cnt < MAX_BLOCKED; ++i) {
        if (Config::DEVICE_POLICY_TABLE[i].blocked)
            blocked_ips[cnt++] = Config::DEVICE_POLICY_TABLE[i].ip;
    }
    blocked_count.store(cnt, std::memory_order_release);
}

void FirewallEngine::sync_blocked_ips() {
    std::lock_guard<std::mutex> lk(Config::device_policy_mutex);
    sync_blocked_ips_locked();
}

bool FirewallEngine::is_blocked_ip(Net::IPv4Net ip) const {
    uint8_t cnt = blocked_count.load(std::memory_order_acquire);
    for (uint8_t i = 0; i < cnt; ++i)
        if (blocked_ips[i] == ip) return true;
    return false;
}

void FirewallEngine::track_outbound(const Net::ParsedPacket& pkt) {
    if (!pkt.is_valid_ipv4()) return;
    uint8_t proto = pkt.l4_protocol;
    if (proto != 6 && proto != 17) return;

    uint32_t remote_ip = pkt.ipv4->daddr.raw();
    uint32_t lan_ip    = pkt.ipv4->saddr.raw();
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

    uint32_t h       = hash_remote(remote_ip, remote_port, proto) % TABLE_SIZE;
    int32_t  free_slot = -1;
    uint32_t tick    = current_tick.load(std::memory_order_relaxed);

    for (size_t i = 0; i < PROBE_LIMIT; ++i) {
        size_t idx = (h + i) % TABLE_SIZE;
        auto& e = table[idx];

        uint32_t s0 = e.seq.load(std::memory_order_acquire);
        if (s0 & 1u) continue;
        uint32_t eri = e.remote_ip;
        uint16_t erp = e.remote_port;
        uint32_t eli = e.lan_ip;
        uint16_t elp = e.lan_port;
        uint8_t epr = e.protocol;
        uint32_t s1 = e.seq.load(std::memory_order_acquire);
        if (s0 != s1 || (s1 & 1u)) continue;

        if (!e.active.load(std::memory_order_acquire) || is_expired(e)) {
            if (free_slot == -1) free_slot = static_cast<int32_t>(idx);
            continue;
        }

        if (eri == remote_ip && erp == remote_port &&
            eli == lan_ip && elp == lan_port && epr == proto) {

            if (proto == 6) {
                uint16_t flags = ntohs(pkt.tcp()->res1_doff_flags);
                if (flags & 0x0004) {
                    e.active.store(false, std::memory_order_release);
                } else if (flags & 0x0001) {
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

    if (free_slot == -1) return;
    auto& ne = table[free_slot];
    ne.seq.fetch_add(1, std::memory_order_acq_rel);
    ne.remote_ip   = remote_ip;
    ne.remote_port = remote_port;
    ne.lan_ip      = lan_ip;
    ne.lan_port    = lan_port;
    ne.protocol    = proto;
    ne.seq.fetch_add(1, std::memory_order_acq_rel);
    ne.last_tick.store(tick, std::memory_order_relaxed);

    if (proto == 6) {
        uint16_t flags = ntohs(pkt.tcp()->res1_doff_flags);
        ne.state.store(((flags & 0x0002) && !(flags & 0x0010))
                       ? ConnState::SYN_SENT
                       : ConnState::ESTABLISHED, std::memory_order_relaxed);
    } else {
        ne.state.store(ConnState::ESTABLISHED, std::memory_order_relaxed);
    }
    ne.active.store(true, std::memory_order_release);
}

bool FirewallEngine::check_inbound(const Net::ParsedPacket& pkt) {
    if (!pkt.is_valid_ipv4()) return false;
    uint8_t proto = pkt.l4_protocol;

    if (proto == 1) return true;
    if (proto != 6 && proto != 17) return false;

    uint32_t remote_ip = pkt.ipv4->saddr.raw();
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

    uint32_t h    = hash_remote(remote_ip, sport, proto) % TABLE_SIZE;
    uint32_t tick = current_tick.load(std::memory_order_relaxed);

    for (size_t i = 0; i < PROBE_LIMIT; ++i) {
        size_t idx = (h + i) % TABLE_SIZE;
        auto& e = table[idx];

        uint32_t s0 = e.seq.load(std::memory_order_acquire);
        if (s0 & 1u) continue;
        bool act = e.active.load(std::memory_order_acquire);
        uint32_t eri = e.remote_ip;
        uint16_t erp = e.remote_port;
        uint8_t epr = e.protocol;
        uint32_t s1 = e.seq.load(std::memory_order_acquire);
        if (s0 != s1 || (s1 & 1u)) continue;
        if (!act || is_expired(e)) continue;
        if (eri != remote_ip || erp != sport || epr != proto) continue;

        if (proto == 17) {
            e.last_tick.store(tick, std::memory_order_relaxed);
            return true;
        }

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
                if (fin) e.state.store(ConnState::FIN_WAIT, std::memory_order_relaxed);
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

void FirewallEngine::cleanup() {
    for (auto& e : table) {
        if (e.active.load(std::memory_order_relaxed) && is_expired(e))
            e.active.store(false, std::memory_order_release);
    }
}

} // namespace HPGTP::Logic
