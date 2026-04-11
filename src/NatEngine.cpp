#include "NatEngine.hpp"
#include <cstring>
#include <netinet/in.h>

namespace Scalpel::Logic {

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

NatEngine::NatEngine() {
    port_to_index.fill(-1);
}

void NatEngine::set_wan_ip(Net::IPv4Net ip) { wan_ip = ip; }

void NatEngine::add_upnp_rule(uint16_t ext_port, Net::IPv4Net int_ip, uint16_t int_port, uint8_t proto) {
    uint16_t net_ext_port = htons(ext_port);
    uint16_t net_int_port = htons(int_port);

    for (auto& rule : upnp_rules) {
        if (rule.active.load(std::memory_order_relaxed)) {
            if (rule.external_port == net_ext_port && rule.protocol == proto) {
                rule.internal_ip   = int_ip;
                rule.internal_port = net_int_port;
                return;
            }
        }
    }
    size_t idx = upnp_cursor.fetch_add(1, std::memory_order_relaxed) % upnp_rules.size();
    upnp_rules[idx].internal_ip   = int_ip;
    upnp_rules[idx].internal_port = net_int_port;
    upnp_rules[idx].external_port = net_ext_port;
    upnp_rules[idx].protocol      = proto;
    upnp_rules[idx].active.store(true, std::memory_order_release);
}

void NatEngine::tick() { current_tick.fetch_add(1, std::memory_order_relaxed); }

bool NatEngine::process_outbound(Net::ParsedPacket& pkt) {
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

    if (upnp_cursor.load(std::memory_order_relaxed) > 0) for (auto& rule : upnp_rules) {
        if (rule.active.load(std::memory_order_acquire)) {
            if (rule.protocol == ip->protocol && rule.internal_ip == ip->saddr && rule.internal_port == *sport_ptr) {
                update_checksum_32(ip->check, ip->saddr, wan_ip);
                if (check_ptr && *check_ptr != 0)
                    update_checksum_32(*check_ptr, ip->saddr, wan_ip);
                ip->saddr  = wan_ip;
                *sport_ptr = rule.external_port;
                return true;
            }
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
            if (is_active) port_to_index[ntohs(sessions[idx].external_port)] = -1;

            sessions[idx].internal_key = key;
            sessions[idx].external_port = htons(port_cursor++);
            if (port_cursor > 60000) port_cursor = 10000;
            sessions[idx].last_active_tick.store(tick, std::memory_order_relaxed);
            sessions[idx].active.store(true, std::memory_order_release);
            ext_port = sessions[idx].external_port;
            port_to_index[ntohs(ext_port)] = static_cast<int32_t>(idx);
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
        if (rule.active.load(std::memory_order_acquire)) {
            if (rule.protocol == ip->protocol && rule.external_port == *dport_ptr) {
                update_checksum_32(ip->check, ip->daddr, rule.internal_ip);
                if (check_ptr && *check_ptr != 0)
                    update_checksum_32(*check_ptr, ip->daddr, rule.internal_ip);
                ip->daddr  = rule.internal_ip;
                *dport_ptr = rule.internal_port;
                return true;
            }
        }
    }

    int32_t idx = port_to_index[ntohs(*dport_ptr)];
    if (idx == -1 || !sessions[idx].active.load(std::memory_order_acquire) ||
        sessions[idx].internal_key.daddr != ip->saddr ||
        sessions[idx].internal_key.dport != sport) {
        return false;
    }

    sessions[idx].last_active_tick.store(
        current_tick.load(std::memory_order_relaxed), std::memory_order_relaxed);
    Net::IPv4Net internal_ip   = sessions[idx].internal_key.saddr;
    uint16_t     internal_port = sessions[idx].internal_key.sport;

    update_checksum_32(ip->check, ip->daddr, internal_ip);
    if (check_ptr && *check_ptr != 0) {
        update_checksum_32(*check_ptr, ip->daddr, internal_ip);
        update_checksum_16(*check_ptr, *dport_ptr, internal_port);
    }

    ip->daddr  = internal_ip;
    *dport_ptr = internal_port;
    return true;
}

} // namespace Scalpel::Logic
