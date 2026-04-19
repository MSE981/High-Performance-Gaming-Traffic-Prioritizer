#include "DnsEngine.hpp"
#include "DataPlane.hpp"
#include <netinet/in.h>
#include <cstring>

namespace HPGTP::Logic {

namespace {

uint32_t hash_qname(const uint8_t* qname, size_t max_len) {
    uint32_t h = 2166136261U;
    size_t i = 0;
    while (i < max_len && qname[i] != 0) { h ^= qname[i]; h *= 16777619U; i++; }
    return h;
}

// RFC 1624 incremental checksum update: HC' = ~(~HC + ~m + m'), stored NBO.
inline void csum_patch_16(uint16_t& check, uint16_t old_val, uint16_t new_val) {
    uint32_t sum = (~ntohs(check) & 0xFFFF) + (~ntohs(old_val) & 0xFFFF) + ntohs(new_val);
    sum = (sum & 0xFFFF) + (sum >> 16);
    check = htons(static_cast<uint16_t>(~(sum + (sum >> 16))));
}
inline void csum_patch_32(uint16_t& check, Net::IPv4Net old_val, Net::IPv4Net new_val) {
    csum_patch_16(check, static_cast<uint16_t>(old_val.raw() & 0xFFFF),
                         static_cast<uint16_t>(new_val.raw() & 0xFFFF));
    csum_patch_16(check, static_cast<uint16_t>(old_val.raw() >> 16),
                         static_cast<uint16_t>(new_val.raw() >> 16));
}

inline uint64_t redirect_key(Net::IPv4Net client_ip, uint16_t port_nbo) {
    return (static_cast<uint64_t>(client_ip.raw()) << 16)
         | static_cast<uint64_t>(port_nbo);
}
inline uint32_t redirect_slot(uint64_t key) {
    uint32_t h = 2166136261U;
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<uint8_t>(key >> (i * 8));
        h *= 16777619U;
    }
    return h;
}

// RFC 1035 name: labels ending in 0, or a suffix pointer (two bytes 11xxxxxx), or pointer-only.
[[nodiscard]] bool skip_one_dns_name(const uint8_t* d, size_t msg_len, size_t& ptr) {
    unsigned guard = 0;
    while (ptr < msg_len && guard++ < 128) {
        if ((d[ptr] & 0xC0) == 0xC0) {
            if (ptr + 2 > msg_len) return false;
            ptr += 2;
            return true;
        }
        const uint8_t lab = d[ptr];
        if (lab == 0) {
            ++ptr;
            return true;
        }
        if (lab > 63 || ptr + 1u + static_cast<size_t>(lab) > msg_len) return false;
        ptr += 1u + lab;
    }
    return false;
}

} // namespace

void DnsEngine::tick() { current_tick.fetch_add(1, std::memory_order_relaxed); }

void DnsEngine::set_upstream(DnsUpstreamConfig cfg) {
    upstream_primary_ip.store(cfg.primary,   std::memory_order_release);
    upstream_secondary_ip.store(cfg.secondary, std::memory_order_release);
}

void DnsEngine::set_redirect(bool enabled) {
    redirect_enabled.store(enabled, std::memory_order_relaxed);
}

void DnsEngine::set_gateway_ip(Net::IPv4Net ip) noexcept {
    gateway_ip.store(ip, std::memory_order_release);
}

void DnsEngine::record_redirect(Net::IPv4Net client_ip, uint16_t sport_nbo,
                                Net::IPv4Net original_dst) noexcept {
    const uint64_t k   = redirect_key(client_ip, sport_nbo);
    const size_t   idx = redirect_slot(k) % REDIRECT_SIZE;
    auto& e = redirect_table[idx];
    e.key.store(0, std::memory_order_release);
    e.original_dst.store(original_dst, std::memory_order_relaxed);
    e.expire_tick.store(
        current_tick.load(std::memory_order_relaxed) + REDIRECT_TTL,
        std::memory_order_relaxed);
    e.key.store(k, std::memory_order_release);
}

bool DnsEngine::lookup_redirect(Net::IPv4Net client_ip, uint16_t dport_nbo,
                                Net::IPv4Net& out_original_dst) noexcept {
    const uint64_t k   = redirect_key(client_ip, dport_nbo);
    const size_t   idx = redirect_slot(k) % REDIRECT_SIZE;
    auto& e = redirect_table[idx];
    if (e.key.load(std::memory_order_acquire) != k) return false;
    const Net::IPv4Net dst = e.original_dst.load(std::memory_order_relaxed);
    const uint32_t     ex  = e.expire_tick.load(std::memory_order_relaxed);
    if (e.key.load(std::memory_order_acquire) != k) return false;
    if (current_tick.load(std::memory_order_relaxed) > ex) return false;
    out_original_dst = dst;
    return true;
}

void DnsEngine::reload_static_records() {
    uint8_t cnt = 0;
    for (size_t i = 0; i < Config::STATIC_DNS_COUNT && cnt < MAX_STATIC; ++i) {
        static_records[cnt].domain_hash = Config::STATIC_DNS_TABLE[i].domain_hash;
        static_records[cnt].ip          = Config::STATIC_DNS_TABLE[i].ip;
        ++cnt;
    }
    static_count.store(cnt, std::memory_order_release);
}

bool DnsEngine::do_bounce(Net::ParsedPacket& pkt, DnsHeader* dns, Net::UDPHeader* udp,
                          Net::IPv4Net ip, int bounce_fd) noexcept {
    if (!pkt.ipv4) return false;
    const size_t eth_sz = sizeof(Net::EthernetHeader);
    const uint16_t ip_tot = ntohs(pkt.ipv4->tot_len);
    if (ip_tot < static_cast<uint16_t>(pkt.ihl)) return false;
    size_t old_len = eth_sz + ip_tot;
    if (old_len > pkt.raw_span.size()) return false;
    size_t new_len = old_len + 16;
    if (new_len > 1500) return false;
    if (pkt.raw_span.size() < new_len) return false;

    dns->flags   = htons(ntohs(dns->flags) | 0x8180);
    dns->ancount = htons(1);

    uint8_t* tail = pkt.raw_span.data() + old_len;
    tail[0] = 0xC0; tail[1] = 0x0C;
    tail[2] = 0x00; tail[3] = 0x01;
    tail[4] = 0x00; tail[5] = 0x01;
    tail[6] = 0x00; tail[7] = 0x00; tail[8] = 0x01; tail[9] = 0x2C;
    tail[10] = 0x00; tail[11] = 0x04;
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

    const auto tr = DataPlane::TxFrameOutput::try_send_packet_nonblocking(
        bounce_fd, std::span<const uint8_t>(pkt.raw_span.data(), new_len));
    if (tr != DataPlane::TxFrameOutput::PacketTxTry::Complete) {
        Telemetry::instance().core_metrics[3].dropped[1].fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void DnsEngine::rewrite_upstream(Net::ParsedPacket& pkt, Net::UDPHeader* udp,
                                 Net::IPv4Net upstream_ip) {
    const Net::IPv4Net old_daddr = pkt.ipv4->daddr;
    pkt.ipv4->daddr = upstream_ip;
    csum_patch_32(pkt.ipv4->check, old_daddr, upstream_ip);
    // UDP pseudo-header includes daddr: patch transport checksum when present.
    if (udp && udp->check != 0)
        csum_patch_32(udp->check, old_daddr, upstream_ip);
}

DnsQueryDisposition DnsEngine::process_query(Net::ParsedPacket& pkt,
                                             int bounce_fd) noexcept {
    if (!pkt.is_valid_ipv4() || pkt.l4_protocol != 17)
        return DnsQueryDisposition::NotHandled;

    auto udp = pkt.udp();
    if (!udp || ntohs(udp->dest) != 53) return DnsQueryDisposition::NotHandled;

    size_t dns_offset = pkt.l4_offset + sizeof(Net::UDPHeader);
    if (pkt.raw_span.size() < dns_offset + sizeof(DnsHeader) + 1)
        return DnsQueryDisposition::NotHandled;

    auto dns = reinterpret_cast<DnsHeader*>(pkt.raw_span.data() + dns_offset);
    if ((ntohs(dns->flags) & 0x8000) != 0) return DnsQueryDisposition::NotHandled;
    if (ntohs(dns->qdcount) != 1) return DnsQueryDisposition::NotHandled;

    size_t qname_offset = dns_offset + sizeof(DnsHeader);
    if (qname_offset >= pkt.raw_span.size()) return DnsQueryDisposition::NotHandled;

    uint32_t h = hash_qname(pkt.raw_span.data() + qname_offset,
                            pkt.raw_span.size() - qname_offset);

    uint8_t scnt = static_count.load(std::memory_order_acquire);
    for (uint8_t i = 0; i < scnt; ++i) {
        if (static_records[i].domain_hash == h)
            return do_bounce(pkt, dns, udp, static_records[i].ip, bounce_fd)
                ? DnsQueryDisposition::Replied
                : DnsQueryDisposition::ReplySendFailed;
    }

    size_t idx = h % CACHE_SIZE;
    for (;;) {
        if (!cache[idx].valid.load(std::memory_order_acquire)) break;
        const uint32_t dh = cache[idx].domain_hash.load(std::memory_order_relaxed);
        const Net::IPv4Net addr = cache[idx].ipv4_address.load(std::memory_order_relaxed);
        const uint32_t ex = cache[idx].expire_tick.load(std::memory_order_relaxed);
        if (!cache[idx].valid.load(std::memory_order_acquire)) continue;
        if (dh == h && current_tick.load(std::memory_order_relaxed) <= ex)
            return do_bounce(pkt, dns, udp, addr, bounce_fd) ? DnsQueryDisposition::Replied
                                                             : DnsQueryDisposition::ReplySendFailed;
        cache[idx].valid.store(false, std::memory_order_relaxed);
        break;
    }

    // Redirect only queries aimed at this router (DHCP-advertised DNS server) to
    // upstream. A record of the original dst is kept so the reply's saddr can be
    // restored on the return path; otherwise the resolver rejects the response.
    const Net::IPv4Net gw = gateway_ip.load(std::memory_order_acquire);
    const Net::IPv4Net upstream = upstream_primary_ip.load(std::memory_order_relaxed);
    if (upstream.raw() != 0 && gw.raw() != 0 && pkt.ipv4->daddr == gw) {
        record_redirect(pkt.ipv4->saddr, udp->source, pkt.ipv4->daddr);
        rewrite_upstream(pkt, udp, upstream);
        return DnsQueryDisposition::Redirected;
    }
    return DnsQueryDisposition::NotHandled;
}

void DnsEngine::process_response(Net::ParsedPacket& pkt) noexcept {
    if (!pkt.is_valid_ipv4() || pkt.l4_protocol != 17) return;

    auto udp = pkt.udp();
    if (!udp || ntohs(udp->source) != 53) return;

    // Restore saddr to the original DNS server IP the client queried. Runs after
    // NAT DNAT so (daddr, dport) equal the client's (ip, sport) recorded on TX.
    Net::IPv4Net orig_dst{};
    if (lookup_redirect(pkt.ipv4->daddr, udp->dest, orig_dst)
        && orig_dst.raw() != 0
        && orig_dst != pkt.ipv4->saddr) {
        const Net::IPv4Net old_saddr = pkt.ipv4->saddr;
        pkt.ipv4->saddr = orig_dst;
        csum_patch_32(pkt.ipv4->check, old_saddr, orig_dst);
        if (udp->check != 0)
            csum_patch_32(udp->check, old_saddr, orig_dst);
    }

    if (pkt.raw_span.size() > 512) return;
    DnsMessage msg;
    msg.len = pkt.raw_span.size();
    std::memcpy(msg.data.data(), pkt.raw_span.data(), pkt.raw_span.size());
    response_queue.push(msg);
}

void DnsEngine::process_background_tasks() {
    DnsMessage msg;
    int counter = 0;
    while (response_queue.pop(msg) && counter++ < 32) {
        auto ip = reinterpret_cast<const Net::IPv4Header*>(msg.data.data() + sizeof(Net::EthernetHeader));
        size_t ihl = (ip->ver_ihl & 0x0F) * 4;
        size_t dns_offset = sizeof(Net::EthernetHeader) + ihl + sizeof(Net::UDPHeader);

        auto dns = reinterpret_cast<const DnsHeader*>(msg.data.data() + dns_offset);
        if (ntohs(dns->ancount) == 0) continue;

        size_t qname_offset = dns_offset + sizeof(DnsHeader);
        if (qname_offset >= msg.len) continue;

        uint32_t h = hash_qname(msg.data.data() + qname_offset, msg.len - qname_offset);

        size_t ptr = qname_offset;
        while (ptr < msg.len) {
            const uint8_t lab = msg.data[ptr];
            if (lab == 0) break;
            if (lab >= 0xC0) {
                ptr = msg.len;
                break;
            }
            if (lab > 63 || ptr + 1u + static_cast<size_t>(lab) > msg.len) {
                ptr = msg.len;
                break;
            }
            ptr += 1u + lab;
        }
        if (ptr >= msg.len || msg.data[ptr] != 0) continue;
        ptr += 1;
        if (ptr + 4 > msg.len) continue;
        ptr += 4;

        if (!skip_one_dns_name(msg.data.data(), msg.len, ptr)) continue;
        if (ptr + 10 > msg.len) continue;

        const uint16_t rtype = static_cast<uint16_t>(
            (static_cast<uint16_t>(msg.data[ptr]) << 8) | msg.data[ptr + 1]);
        const uint16_t rdlen = static_cast<uint16_t>(
            (static_cast<uint16_t>(msg.data[ptr + 8]) << 8) | msg.data[ptr + 9]);
        if (rtype != 1 || rdlen != 4) continue;
        if (ptr + 10 + 4 > msg.len) continue;

        Net::IPv4Net ipv4{*reinterpret_cast<const uint32_t*>(&msg.data[ptr + 10])};

        size_t idx = h % CACHE_SIZE;
        cache[idx].valid.store(false, std::memory_order_release);
        cache[idx].domain_hash.store(h, std::memory_order_relaxed);
        cache[idx].ipv4_address.store(ipv4, std::memory_order_relaxed);
        cache[idx].expire_tick.store(
            current_tick.load(std::memory_order_relaxed) + 300, std::memory_order_relaxed);
        cache[idx].valid.store(true, std::memory_order_release);
    }
}

} // namespace HPGTP::Logic
