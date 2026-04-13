// firewall_demo: verify conntrack state machine and blocked-IP enforcement
//
// Build: make firewall_demo
// Run:   ./firewall_demo   (no root required)
#include "FirewallEngine.hpp"
#include "Headers.hpp"

namespace Net = Scalpel::Net;

#include <print>
#include <cassert>
#include <cstring>
#include <netinet/in.h>

// Build a minimal TCP frame
static std::array<uint8_t, 60> make_tcp_frame(
    uint32_t src, uint32_t dst,
    uint16_t sport, uint16_t dport,
    uint16_t tcp_flags)  // host-byte-order flags word
{
    std::array<uint8_t, 60> buf{};
    buf[12] = 0x08; buf[13] = 0x00;

    auto* ip    = reinterpret_cast<Net::IPv4Header*>(&buf[14]);
    ip->ver_ihl  = 0x45;
    ip->tot_len  = htons(46);  // 20 IP + 20 TCP + 6 pad
    ip->protocol = 6;
    ip->saddr    = Net::IPv4Net{src};
    ip->daddr    = Net::IPv4Net{dst};

    auto* tcp                  = reinterpret_cast<Net::TCPHeader*>(&buf[34]);
    tcp->source                = htons(sport);
    tcp->dest                  = htons(dport);
    tcp->res1_doff_flags       = htons(tcp_flags | (5u << 12));  // data offset = 5
    return buf;
}

int main() {
    std::println("=== Firewall Engine Demo ===");

    Scalpel::Logic::FirewallEngine fw;

    const uint32_t lan = htonl(0xC0A80101);  // 192.168.1.1
    const uint32_t srv = htonl(0x08080808);  // 8.8.8.8

    // ── 1. Outbound SYN → creates SYN_SENT entry ─────────────────────────────
    {
        auto f   = make_tcp_frame(lan, srv, 50000, 443, 0x0002);  // SYN
        auto pkt = Net::ParsedPacket::parse(std::span<uint8_t>{f});
        fw.track_outbound(pkt);
        std::println("[INFO] Outbound SYN registered.");
    }

    // ── 2. Inbound SYN-ACK → check_inbound should allow and transition ────────
    {
        auto f   = make_tcp_frame(srv, lan, 443, 50000, 0x0012);  // SYN+ACK
        auto pkt = Net::ParsedPacket::parse(std::span<uint8_t>{f});
        bool allowed = fw.check_inbound(pkt);
        assert(allowed && "SYN-ACK from known server must be allowed");
        std::println("[PASS] Inbound SYN-ACK allowed (SYN_SENT → ESTABLISHED).");
    }

    // ── 3. Unsolicited inbound SYN → must be dropped ─────────────────────────
    {
        const uint32_t attacker = htonl(0xC0A80199);  // 192.168.1.153
        auto f   = make_tcp_frame(attacker, lan, 9999, 80, 0x0002);  // SYN
        auto pkt = Net::ParsedPacket::parse(std::span<uint8_t>{f});
        bool blocked = fw.check_inbound(pkt);
        assert(!blocked && "Unsolicited inbound SYN must be dropped");
        std::println("[PASS] Unsolicited inbound SYN dropped.");
    }

    // ── 4. Blocked IP enforcement ─────────────────────────────────────────────
    {
        const Net::IPv4Net bad_ip{htonl(0xC0A801FE)};  // 192.168.1.254
        // Inject one blocked entry directly via Config
        Scalpel::Config::DEVICE_POLICY_TABLE[0].ip      = bad_ip;
        Scalpel::Config::DEVICE_POLICY_TABLE[0].blocked = true;
        Scalpel::Config::DEVICE_POLICY_COUNT             = 1;
        fw.sync_blocked_ips();

        assert(fw.is_blocked_ip(bad_ip) && "Bad IP must be in block list");
        std::println("[PASS] Blocked IP enforcement verified.");
    }

    std::println("=== Done ===");
    return 0;
}
