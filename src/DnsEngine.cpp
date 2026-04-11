#include "DnsEngine.hpp"
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#include <print>

namespace Scalpel::Logic {

static uint32_t hash_qname(const uint8_t* qname, size_t max_len) {
    uint32_t h = 2166136261U;
    size_t i = 0;
    while (i < max_len && qname[i] != 0) { h ^= qname[i]; h *= 16777619U; i++; }
    return h;
}

void DnsEngine::tick() { current_tick.fetch_add(1, std::memory_order_relaxed); }

void DnsEngine::set_upstream(DnsUpstreamConfig cfg) {
    upstream_primary_ip.store(cfg.primary,   std::memory_order_release);
    upstream_secondary_ip.store(cfg.secondary, std::memory_order_release);
}

void DnsEngine::set_redirect(bool enabled) {
    redirect_enabled.store(enabled, std::memory_order_relaxed);
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

void DnsEngine::do_bounce(Net::ParsedPacket& pkt, DnsHeader* dns, Net::UDPHeader* udp,
                          Net::IPv4Net ip, int bounce_fd) {
    size_t old_len = pkt.raw_span.size();
    size_t new_len = old_len + 16;
    if (new_len > 1500) return;

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

    if (send(bounce_fd, pkt.raw_span.data(), new_len, MSG_DONTWAIT) < 0)
        Telemetry::instance().core_metrics[3].dropped[1].fetch_add(1, std::memory_order_relaxed);
}

void DnsEngine::rewrite_upstream(Net::ParsedPacket& pkt, Net::IPv4Net upstream_ip) {
    pkt.ipv4->daddr = upstream_ip;
    pkt.ipv4->check = 0;
    uint32_t sum = 0;
    const uint16_t* words = reinterpret_cast<const uint16_t*>(pkt.ipv4);
    for (size_t i = 0; i < pkt.ihl / 2; ++i) sum += ntohs(words[i]);
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    pkt.ipv4->check = htons(~static_cast<uint16_t>(sum));
}

bool DnsEngine::process_query(Net::ParsedPacket& pkt, int bounce_fd) {
    if (!pkt.is_valid_ipv4() || pkt.l4_protocol != 17) return false;

    auto udp = pkt.udp();
    if (!udp || ntohs(udp->dest) != 53) return false;

    size_t dns_offset = pkt.l4_offset + sizeof(Net::UDPHeader);
    if (pkt.raw_span.size() < dns_offset + sizeof(DnsHeader) + 1) return false;

    auto dns = reinterpret_cast<DnsHeader*>(pkt.raw_span.data() + dns_offset);
    if ((ntohs(dns->flags) & 0x8000) != 0) return false;
    if (ntohs(dns->qdcount) != 1) return false;

    size_t qname_offset = dns_offset + sizeof(DnsHeader);
    if (qname_offset >= pkt.raw_span.size()) return false;

    uint32_t h = hash_qname(pkt.raw_span.data() + qname_offset,
                            pkt.raw_span.size() - qname_offset);

    uint8_t scnt = static_count.load(std::memory_order_acquire);
    for (uint8_t i = 0; i < scnt; ++i) {
        if (static_records[i].domain_hash == h) {
            do_bounce(pkt, dns, udp, static_records[i].ip, bounce_fd);
            return true;
        }
    }

    size_t idx = h % CACHE_SIZE;
    if (cache[idx].valid.load(std::memory_order_acquire)) {
        if (cache[idx].domain_hash == h &&
            current_tick.load(std::memory_order_relaxed) <= cache[idx].expire_tick) {
            do_bounce(pkt, dns, udp, cache[idx].ipv4_address, bounce_fd);
            return true;
        }
        cache[idx].valid.store(false, std::memory_order_relaxed);
    }

    if (redirect_enabled.load(std::memory_order_relaxed)) {
        Net::IPv4Net upstream = upstream_primary_ip.load(std::memory_order_relaxed);
        if (upstream.raw() != 0) rewrite_upstream(pkt, upstream);
    }
    return false;
}

void DnsEngine::intercept_response(const Net::ParsedPacket& pkt) {
    if (pkt.raw_span.size() > 512 || !pkt.is_valid_ipv4()) return;
    if (pkt.l4_protocol != 17) return;

    auto udp = pkt.udp();
    if (!udp || ntohs(udp->source) != 53) return;

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
        while (ptr < msg.len && msg.data[ptr] != 0) { ptr += msg.data[ptr] + 1; }
        ptr += 5;

        if (ptr + 12 <= msg.len) {
            if (msg.data[ptr+2] == 0x00 && msg.data[ptr+3] == 0x01) {
                Net::IPv4Net ipv4{*reinterpret_cast<const uint32_t*>(&msg.data[ptr+10])};

                size_t idx = h % CACHE_SIZE;
                cache[idx].domain_hash  = h;
                cache[idx].ipv4_address = ipv4;
                cache[idx].expire_tick  = current_tick.load(std::memory_order_relaxed) + 300;
                cache[idx].valid.store(true, std::memory_order_release);
            }
        }
    }
}

} // namespace Scalpel::Logic
