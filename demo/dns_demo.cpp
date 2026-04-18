// dns_demo: verify static DNS record injection and cache-hit bounce
//
// Build: make dns_demo
// Run:   ./dns_demo   (no root required)
#include "DnsEngine.hpp"
#include "Config.hpp"
#include "Headers.hpp"

namespace Net = HPGTP::Net;

#include <print>
#include <cassert>
#include <cstring>
#include <netinet/in.h>

// Encode dotted hostname to DNS QNAME wire format.
// Returns byte length of the encoded QNAME (including trailing zero).
static size_t encode_qname(const char* host, uint8_t* out, size_t cap) {
    size_t pos = 0;
    const char* p = host;
    while (*p && pos + 2 < cap) {
        size_t lstart = pos++;
        uint8_t len   = 0;
        while (*p && *p != '.' && pos < cap - 1) {
            out[pos++] = static_cast<uint8_t>(*p++);
            ++len;
        }
        out[lstart] = len;
        if (*p == '.') ++p;
    }
    if (pos < cap) out[pos++] = 0;
    return pos;
}

// Build a minimal Ethernet+IPv4+UDP+DNS query frame for the given hostname.
static std::vector<uint8_t> make_dns_query(const char* hostname) {
    uint8_t qname[256]{};
    size_t qname_len = encode_qname(hostname, qname, sizeof(qname));

    // Frame: 14 Eth + 20 IP + 8 UDP + 12 DNS hdr + qname + 4 (type+class)
    size_t dns_payload = 12 + qname_len + 4;
    size_t total       = 14 + 20 + 8 + dns_payload;
    std::vector<uint8_t> buf(total + 64, 0);

    // Ethernet
    buf[12] = 0x08; buf[13] = 0x00;

    // IPv4
    auto* ip    = reinterpret_cast<Net::IPv4Header*>(&buf[14]);
    ip->ver_ihl  = 0x45;
    ip->tot_len  = htons(static_cast<uint16_t>(total - 14));
    ip->protocol = 17;
    ip->saddr    = Net::IPv4Net{htonl(0xC0A80101)};  // 192.168.1.1
    ip->daddr    = Net::IPv4Net{htonl(0xC0A80101)};  // loopback-style dst

    // UDP
    auto* udp    = reinterpret_cast<Net::UDPHeader*>(&buf[34]);
    udp->source  = htons(12345);
    udp->dest    = htons(53);
    udp->len     = htons(static_cast<uint16_t>(8 + dns_payload));

    // DNS header
    uint8_t* dns_hdr = &buf[42];
    dns_hdr[0] = 0xAB; dns_hdr[1] = 0xCD;  // ID
    dns_hdr[4] = 0x00; dns_hdr[5] = 0x01;  // qdcount = 1

    // QNAME
    uint8_t* qptr = dns_hdr + 12;
    std::memcpy(qptr, qname, qname_len);
    qptr += qname_len;
    // QTYPE A, QCLASS IN
    qptr[0] = 0x00; qptr[1] = 0x01;
    qptr[2] = 0x00; qptr[3] = 0x01;

    return buf;
}

int main() {
    std::println("=== DNS Engine Demo ===");

    // Inject a static record: example.test → 10.0.0.1
    HPGTP::Config::upsert_static_dns("example.test", "10.0.0.1");

    HPGTP::Logic::DnsEngine dns;
    dns.reload_static_records();

    auto frame  = make_dns_query("example.test");
    auto pkt    = Net::ParsedPacket::parse(std::span<uint8_t>(frame.data(), frame.size()));

    using HPGTP::Logic::DnsQueryDisposition;
    // bounce_fd = -1: response is built and send fails → ReplySendFailed (not Replied).
    const DnsQueryDisposition hit = dns.process_query(pkt, -1);
    assert(hit == DnsQueryDisposition::ReplySendFailed &&
           "Static hit must classify as ReplySendFailed when bounce fd is invalid");

    std::println("[PASS] process_query returned ReplySendFailed for static record (invalid fd).");

    auto frame2 = make_dns_query("unknown.test");
    auto pkt2   = Net::ParsedPacket::parse(std::span<uint8_t>(frame2.data(), frame2.size()));
    const DnsQueryDisposition miss = dns.process_query(pkt2, -1);
    assert(miss == DnsQueryDisposition::NotHandled &&
           "Unknown hostname should be NotHandled (forward to upstream)");

    std::println("[PASS] process_query returned NotHandled for unknown hostname.");
    std::println("=== Done ===");
    return 0;
}
