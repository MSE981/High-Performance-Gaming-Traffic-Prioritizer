#include "NatEngine.hpp"
#include <cstring>
#include <netinet/in.h>

namespace HPGTP::Logic {

// Incremental checksum update (RFC 1624): HC' = ~(~HC + ~m + m')
static void update_checksum_16(uint16_t& check, uint16_t old_val, uint16_t new_val) {
    uint32_t sum = (~ntohs(check) & 0xFFFF) + (~ntohs(old_val) & 0xFFFF) + ntohs(new_val);
    sum = (sum & 0xFFFF) + (sum >> 16);
    check = htons(~(sum + (sum >> 16)));
}

static void update_checksum_32(uint16_t& check, Net::IPv4Net old_val, Net::IPv4Net new_val) {
    update_checksum_16(check, old_val.raw() & 0xFFFF, new_val.raw() & 0xFFFF);
    update_checksum_16(check, old_val.raw() >> 16,    new_val.raw() >> 16);
}

uint32_t NatEngine::hash_flow(const FlowKey& k) const {
    uint32_t h = 2166136261U;
    auto proc = [&](const auto& val) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
        for (size_t i = 0; i < sizeof(val); ++i) { h ^= p[i]; h *= 16777619U; }
    };
    proc(k.saddr); proc(k.daddr); proc(k.sport); proc(k.dport);
    return h;
}

uint32_t NatEngine::hash_icmp_flow(Net::IPv4Net sa, Net::IPv4Net da, uint16_t id_nbo) const {
    uint32_t h = 2166136261U;
    auto proc = [&](const auto& val) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
        for (size_t i = 0; i < sizeof(val); ++i) { h ^= p[i]; h *= 16777619U; }
    };
    proc(sa);
    proc(da);
    proc(id_nbo);
    return h;
}

uint16_t NatEngine::alloc_external_icmp_id() noexcept {
    for (int n = 0; n < 512; ++n) {
        const uint16_t host = icmp_id_cursor++;
        if (icmp_id_cursor > 60000) icmp_id_cursor = 10000;
        const int32_t idx = icmp_id_to_index[host].load(std::memory_order_acquire);
        if (idx < 0) return htons(host);
        if (static_cast<size_t>(idx) < MAX_ICMP_SESSIONS
            && !icmp_sessions[static_cast<size_t>(idx)].active.load(std::memory_order_acquire)) {
            int32_t expect = static_cast<int32_t>(idx);
            (void)icmp_id_to_index[host].compare_exchange_strong(
                expect, -1, std::memory_order_release, std::memory_order_relaxed);
            return htons(host);
        }
    }
    return 0;
}

bool NatEngine::process_outbound_icmp(Net::ParsedPacket& pkt) {
    auto ip = pkt.ipv4;
    const Net::IPv4Net wan_ip{wan_ip_nbo.load(std::memory_order_acquire)};
    if (wan_ip.raw() == 0) return false;

    auto icmp = pkt.icmp_echo();
    if (!icmp) return false;
    if (ntohs(ip->frag_off) & 0x3FFF) return false;
    if (icmp->type != 8 || icmp->code != 0) return false;

    const uint16_t id_nbo = icmp->id;
    const uint32_t h    = hash_icmp_flow(ip->saddr, ip->daddr, id_nbo) % MAX_ICMP_SESSIONS;
    const uint32_t tick = current_tick.load(std::memory_order_relaxed);
    uint16_t       ext_nbo = 0;

    for (size_t n = 0; n < 32; ++n) {
        const size_t idx      = (h + n) % MAX_ICMP_SESSIONS;
        const bool   is_active = icmp_sessions[idx].active.load(std::memory_order_acquire);
        const bool   expired =
            is_active
            && (tick - icmp_sessions[idx].last_active_tick.load(std::memory_order_relaxed) > 300);

        if (!is_active || expired) {
            IcmpEchoSession& sess = icmp_sessions[idx];
            sess.seq.fetch_add(1, std::memory_order_acq_rel);
            if (is_active) {
                const uint16_t oh = ntohs(sess.ext_id_nbo);
                int32_t        exp = static_cast<int32_t>(idx);
                (void)icmp_id_to_index[oh].compare_exchange_strong(
                    exp, -1, std::memory_order_release, std::memory_order_relaxed);
                sess.active.store(false, std::memory_order_release);
            }
            const uint16_t new_ext = alloc_external_icmp_id();
            if (!new_ext) {
                sess.seq.fetch_add(1, std::memory_order_acq_rel);
                return false;
            }
            sess.int_saddr     = ip->saddr;
            sess.remote_daddr  = ip->daddr;
            sess.int_id_nbo    = id_nbo;
            sess.ext_id_nbo    = new_ext;
            sess.last_active_tick.store(tick, std::memory_order_relaxed);
            icmp_id_to_index[ntohs(new_ext)].store(static_cast<int32_t>(idx), std::memory_order_release);
            sess.seq.fetch_add(1, std::memory_order_acq_rel);
            sess.active.store(true, std::memory_order_release);
            ext_nbo = new_ext;
            break;
        }

        if (icmp_sessions[idx].int_saddr == ip->saddr
            && icmp_sessions[idx].remote_daddr == ip->daddr
            && icmp_sessions[idx].int_id_nbo == id_nbo) {
            icmp_sessions[idx].last_active_tick.store(tick, std::memory_order_relaxed);
            ext_nbo = icmp_sessions[idx].ext_id_nbo;
            break;
        }
    }

    if (!ext_nbo) return false;

    const uint16_t old_id = icmp->id;
    update_checksum_32(ip->check, ip->saddr, wan_ip);
    if (icmp->check != 0)
        update_checksum_16(icmp->check, old_id, ext_nbo);
    ip->saddr = wan_ip;
    icmp->id  = ext_nbo;
    return true;
}

bool NatEngine::process_inbound_icmp(Net::ParsedPacket& pkt) {
    auto ip = pkt.ipv4;
    const Net::IPv4Net wan_ip{wan_ip_nbo.load(std::memory_order_acquire)};
    if (ip->daddr != wan_ip) return false;

    auto icmp = pkt.icmp_echo();
    if (!icmp) return false;
    if (ntohs(ip->frag_off) & 0x3FFF) return false;
    if (icmp->type != 0 || icmp->code != 0) return false;

    const uint16_t     ext_host = ntohs(icmp->id);
    const int32_t      idx      = icmp_id_to_index[ext_host].load(std::memory_order_acquire);
    if (idx < 0 || static_cast<size_t>(idx) >= MAX_ICMP_SESSIONS) return false;

    Net::IPv4Net int_sa{};
    uint16_t     int_id = 0;
    bool         resolved = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        IcmpEchoSession& sess = icmp_sessions[static_cast<size_t>(idx)];
        uint32_t         s0   = sess.seq.load(std::memory_order_acquire);
        if (s0 & 1u) continue;
        const bool        act      = sess.active.load(std::memory_order_acquire);
        const Net::IPv4Net rem     = sess.remote_daddr;
        const Net::IPv4Net int_sa_c = sess.int_saddr;
        const uint16_t     int_id_c = sess.int_id_nbo;
        const uint16_t     ext_id_c = sess.ext_id_nbo;
        uint32_t           s1       = sess.seq.load(std::memory_order_acquire);
        if (s0 != s1 || (s1 & 1u)) continue;
        if (!act) return false;
        if (rem != ip->saddr || ext_id_c != icmp->id) return false;
        int_sa    = int_sa_c;
        int_id    = int_id_c;
        resolved  = true;
        break;
    }
    if (!resolved) return false;

    const uint16_t old_icmp_id = icmp->id;
    update_checksum_32(ip->check, ip->daddr, int_sa);
    if (icmp->check != 0)
        update_checksum_16(icmp->check, old_icmp_id, int_id);
    ip->daddr = int_sa;
    icmp->id  = int_id;

    icmp_sessions[static_cast<size_t>(idx)].last_active_tick.store(
        current_tick.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return true;
}

NatEngine::NatEngine() {
    for (auto& p : port_to_index)
        p.store(-1, std::memory_order_relaxed);
    for (auto& p : icmp_id_to_index)
        p.store(-1, std::memory_order_relaxed);
}

void NatEngine::set_wan_ip(Net::IPv4Net ip) {
    wan_ip_nbo.store(ip.raw(), std::memory_order_release);
}

void NatEngine::add_upnp_rule(UpnpRule rule) {
    uint16_t net_ext_port = htons(rule.ext_port);
    uint16_t net_int_port = htons(rule.int_port);

    for (auto& r : upnp_rules) {
        if (r.active.load(std::memory_order_acquire)) {
            if (r.external_port == net_ext_port && r.protocol == rule.proto) {
                r.seq.fetch_add(1, std::memory_order_acq_rel);
                r.internal_ip   = rule.int_ip;
                r.internal_port = net_int_port;
                r.seq.fetch_add(1, std::memory_order_acq_rel);
                return;
            }
        }
    }
    size_t idx = upnp_cursor.fetch_add(1, std::memory_order_relaxed) % upnp_rules.size();
    auto& slot = upnp_rules[idx];
    slot.seq.fetch_add(1, std::memory_order_acq_rel);
    slot.internal_ip   = rule.int_ip;
    slot.internal_port = net_int_port;
    slot.external_port = net_ext_port;
    slot.protocol      = rule.proto;
    slot.seq.fetch_add(1, std::memory_order_acq_rel);
    slot.active.store(true, std::memory_order_release);
}

void NatEngine::tick() { current_tick.fetch_add(1, std::memory_order_relaxed); }

bool NatEngine::process_outbound(Net::ParsedPacket& pkt) {
    if (!pkt.is_valid_ipv4()) return false;
    const Net::IPv4Net wan_ip{wan_ip_nbo.load(std::memory_order_acquire)};
    if (wan_ip.raw() == 0) return false;

    auto ip = pkt.ipv4;
    if (ip->protocol == 1) return process_outbound_icmp(pkt);
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

    if (upnp_cursor.load(std::memory_order_relaxed) > 0) for (auto& rule : upnp_rules) {
        if (!rule.active.load(std::memory_order_acquire)) continue;
        uint32_t s0 = rule.seq.load(std::memory_order_acquire);
        if (s0 & 1u) continue;
        Net::IPv4Net r_int_ip   = rule.internal_ip;
        uint16_t     r_int_port = rule.internal_port;
        uint16_t     r_ext_port = rule.external_port;
        uint8_t      r_prot     = rule.protocol;
        uint32_t s1 = rule.seq.load(std::memory_order_acquire);
        if (s0 != s1 || (s1 & 1u)) continue;
        if (r_prot == ip->protocol && r_int_ip == ip->saddr && r_int_port == *sport_ptr) {
            update_checksum_32(ip->check, ip->saddr, wan_ip);
            if (check_ptr && *check_ptr != 0)
                update_checksum_32(*check_ptr, ip->saddr, wan_ip);
            ip->saddr  = wan_ip;
            *sport_ptr = r_ext_port;
            return true;
        }
    }

    FlowKey key{ip->saddr, ip->daddr, *sport_ptr, dport};
    uint32_t h    = hash_flow(key) % MAX_SESSIONS;
    uint32_t tick = current_tick.load(std::memory_order_relaxed);
    uint16_t ext_port = 0;

    for (size_t i = 0; i < 32; ++i) {
        size_t idx   = (h + i) % MAX_SESSIONS;
        bool is_active = sessions[idx].active.load(std::memory_order_acquire);
        if (!is_active || (tick - sessions[idx].last_active_tick.load(std::memory_order_relaxed) > 300)) {
            NatSession& sess = sessions[idx];
            sess.seq.fetch_add(1, std::memory_order_acq_rel);
            if (is_active) {
                const uint16_t old_host = ntohs(sess.external_port);
                int32_t expect_idx = static_cast<int32_t>(idx);
                (void)port_to_index[old_host].compare_exchange_strong(
                    expect_idx, -1, std::memory_order_release, std::memory_order_relaxed);
            }
            sess.internal_key = key;
            const uint16_t ext_nbo = htons(port_cursor++);
            if (port_cursor > 60000) port_cursor = 10000;
            sess.external_port = ext_nbo;
            sess.last_active_tick.store(tick, std::memory_order_relaxed);
            sess.seq.fetch_add(1, std::memory_order_acq_rel);
            sess.active.store(true, std::memory_order_release);
            ext_port = ext_nbo;
            port_to_index[ntohs(ext_port)].store(static_cast<int32_t>(idx),
                                                 std::memory_order_release);
            break;
        }
        if (sessions[idx].internal_key == key) {
            sessions[idx].last_active_tick.store(tick, std::memory_order_relaxed);
            ext_port = sessions[idx].external_port;
            break;
        }
    }

    if (!ext_port) return false;

    update_checksum_32(ip->check, ip->saddr, wan_ip);
    if (check_ptr && *check_ptr != 0) {
        update_checksum_32(*check_ptr, ip->saddr, wan_ip);
        update_checksum_16(*check_ptr, *sport_ptr, ext_port);
    }

    ip->saddr  = wan_ip;
    *sport_ptr = ext_port;
    return true;
}

bool NatEngine::process_inbound(Net::ParsedPacket& pkt) {
    if (!pkt.is_valid_ipv4()) return false;
    auto ip = pkt.ipv4;
    const Net::IPv4Net wan_ip{wan_ip_nbo.load(std::memory_order_acquire)};

    if (ip->protocol == 1) return process_inbound_icmp(pkt);
    if (ip->protocol != 6 && ip->protocol != 17) return false;
    if (ip->daddr != wan_ip) return false;

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

    if (upnp_cursor.load(std::memory_order_relaxed) > 0) for (auto& rule : upnp_rules) {
        if (!rule.active.load(std::memory_order_acquire)) continue;
        uint32_t s0 = rule.seq.load(std::memory_order_acquire);
        if (s0 & 1u) continue;
        Net::IPv4Net r_int_ip   = rule.internal_ip;
        uint16_t     r_int_port = rule.internal_port;
        uint16_t     r_ext_port = rule.external_port;
        uint8_t      r_prot     = rule.protocol;
        uint32_t s1 = rule.seq.load(std::memory_order_acquire);
        if (s0 != s1 || (s1 & 1u)) continue;
        if (r_prot == ip->protocol && r_ext_port == *dport_ptr) {
            update_checksum_32(ip->check, ip->daddr, r_int_ip);
            if (check_ptr && *check_ptr != 0)
                update_checksum_32(*check_ptr, ip->daddr, r_int_ip);
            ip->daddr  = r_int_ip;
            *dport_ptr = r_int_port;
            return true;
        }
    }

    const uint16_t dport_host = ntohs(*dport_ptr);
    int32_t idx = port_to_index[dport_host].load(std::memory_order_acquire);
    if (idx < 0 || static_cast<size_t>(idx) >= MAX_SESSIONS) return false;

    bool         resolved = false;
    Net::IPv4Net internal_ip{};
    uint16_t     internal_port = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        NatSession& sess = sessions[static_cast<size_t>(idx)];
        uint32_t s0 = sess.seq.load(std::memory_order_acquire);
        if (s0 & 1u) continue;
        bool act = sess.active.load(std::memory_order_acquire);
        FlowKey ik = sess.internal_key;
        uint16_t ep = sess.external_port;
        uint32_t s1 = sess.seq.load(std::memory_order_acquire);
        if (s0 != s1 || (s1 & 1u)) continue;
        if (!act) return false;
        if (ik.daddr != ip->saddr || ik.dport != sport || ep != *dport_ptr) return false;
        internal_ip   = ik.saddr;
        internal_port = ik.sport;
        sess.last_active_tick.store(
            current_tick.load(std::memory_order_relaxed), std::memory_order_relaxed);
        resolved = true;
        break;
    }
    if (!resolved) return false;

    update_checksum_32(ip->check, ip->daddr, internal_ip);
    if (check_ptr && *check_ptr != 0) {
        update_checksum_32(*check_ptr, ip->daddr, internal_ip);
        update_checksum_16(*check_ptr, *dport_ptr, internal_port);
    }

    ip->daddr  = internal_ip;
    *dport_ptr = internal_port;
    return true;
}

} // namespace HPGTP::Logic
