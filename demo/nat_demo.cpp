// nat_demo: verify SNAT rewrite on a hand-crafted outbound packet
//
// Build via CMake (from build/ directory):
//   cmake .. && make nat_demo
// Run (no root required — pure in-memory test):
//   ./nat_demo
#include "NatEngine.hpp"
#include "Headers.hpp"
#include <cstring>
#include <print>
#include <netinet/in.h>
#include <cassert>

// Build a minimal Ethernet+IPv4+UDP frame in a byte array
static std::array<uint8_t, 56> make_udp_frame(
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t sport,  uint16_t dport)
{
    std::array<uint8_t, 56> buf{};

    // Ethernet header (14 bytes) — dummy MACs, IPv4 ethertype
    buf[12] = 0x08; buf[13] = 0x00;

    // IPv4 header (20 bytes) at offset 14
    auto* ip = reinterpret_cast<Net::IPv4Header*>(&buf[14]);
    ip->ver_ihl  = 0x45;  // version 4, IHL 5
    ip->tos      = 0;
    ip->tot_len  = htons(34);  // 20 IP + 8 UDP + 6 payload
    ip->id       = 0;
    ip->frag_off = 0;
    ip->ttl      = 64;
    ip->protocol = 17;  // UDP
    ip->check    = 0;
    ip->saddr    = Net::IPv4Net{src_ip};
    ip->daddr    = Net::IPv4Net{dst_ip};

    // UDP header (8 bytes) at offset 34
    auto* udp = reinterpret_cast<Net::UDPHeader*>(&buf[34]);
    udp->source = htons(sport);
    udp->dest   = htons(dport);
    udp->len    = htons(14);
    udp->check  = 0;

    return buf;
}

int main() {
    std::println("=== NAT Engine Demo ===");

    Scalpel::Logic::NatEngine nat;

    // WAN IP: 203.0.113.1 (TEST-NET-3, RFC 5737)
    const uint32_t wan_raw = htonl(0xCB007101);  // 203.0.113.1 in NBO
    nat.set_wan_ip(Net::IPv4Net{wan_raw});

    // LAN client: 192.168.1.42, port 54321 → external server 8.8.8.8:53
    const uint32_t lan_raw = htonl(0xC0A8012A);  // 192.168.1.42
    const uint32_t ext_raw = htonl(0x08080808);  // 8.8.8.8

    auto frame = make_udp_frame(lan_raw, ext_raw, 54321, 53);
    auto pkt   = Net::ParsedPacket::parse(std::span<uint8_t>{frame});

    std::println("Before SNAT: src={:#010x} sport={}",
                 pkt.ipv4->saddr.raw(), ntohs(pkt.udp()->source));

    bool ok = nat.process_outbound(pkt);
    assert(ok && "process_outbound should succeed");

    std::println("After SNAT:  src={:#010x} sport={}",
                 pkt.ipv4->saddr.raw(), ntohs(pkt.udp()->source));

    assert(pkt.ipv4->saddr == Net::IPv4Net{wan_raw} && "saddr must be rewritten to WAN IP");
    assert(ntohs(pkt.udp()->source) >= 10000 && "external port must be in NAT range");

    std::println("[PASS] Outbound SNAT rewrite verified.");

    // Simulate inbound reply — build reversed frame
    auto reply = make_udp_frame(ext_raw, wan_raw,
                                53, ntohs(pkt.udp()->source));
    auto rpkt  = Net::ParsedPacket::parse(std::span<uint8_t>{reply});
    bool rok   = nat.process_inbound(rpkt);
    assert(rok && "process_inbound should match the session");
    assert(rpkt.ipv4->daddr == Net::IPv4Net{lan_raw} && "daddr must be restored to LAN IP");

    std::println("[PASS] Inbound DNAT reverse rewrite verified.");
    std::println("=== Done ===");
    return 0;
}
