// dhcp_demo: verify DHCP DISCOVER → OFFER round-trip in process memory
//
// Build: make dhcp_demo
// Run:   ./dhcp_demo   (no root required — uses socketpair(AF_UNIX))
#include "DhcpEngine.hpp"
#include "Headers.hpp"

namespace Net = Scalpel::Net;

#include <print>
#include <cassert>
#include <cstring>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// Wire-format DHCP header (same layout as DhcpEngine.cpp's internal type)
#pragma pack(push, 1)
struct DhcpWireHdr {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
};
#pragma pack(pop)

// Build a minimal Ethernet+IPv4+UDP+DHCP DISCOVER frame
static std::array<uint8_t, 512> make_discover(size_t& out_len) {
    std::array<uint8_t, 512> buf{};

    // Ethernet (14) + IPv4 (20) + UDP (8)
    buf[12] = 0x08; buf[13] = 0x00;              // EtherType IPv4
    auto* ip  = reinterpret_cast<Net::IPv4Header*>(&buf[14]);
    auto* udp = reinterpret_cast<Net::UDPHeader*>(&buf[34]);

    ip->ver_ihl  = 0x45;
    ip->protocol = 17;  // UDP
    ip->saddr    = Net::IPv4Net{};             // 0.0.0.0
    ip->daddr    = Net::IPv4Net{0xFFFFFFFF};   // broadcast
    udp->source  = htons(68);
    udp->dest    = htons(67);

    // DHCP header at byte 42
    auto* dhcp         = reinterpret_cast<DhcpWireHdr*>(&buf[42]);
    dhcp->op           = 1;               // BootRequest
    dhcp->htype        = 1;               // Ethernet
    dhcp->hlen         = 6;
    dhcp->xid          = htonl(0xCAFEBABE);
    dhcp->magic_cookie = htonl(0x63825363);
    // Synthetic client MAC: DE:AD:BE:EF:00:01
    dhcp->chaddr[0] = 0xDE; dhcp->chaddr[1] = 0xAD;
    dhcp->chaddr[2] = 0xBE; dhcp->chaddr[3] = 0xEF;
    dhcp->chaddr[4] = 0x00; dhcp->chaddr[5] = 0x01;

    // DHCP options
    size_t opt = 42 + sizeof(DhcpWireHdr);
    buf[opt++] = 53; buf[opt++] = 1; buf[opt++] = 1; // Msg-Type = DISCOVER
    buf[opt++] = 255;                                 // End

    out_len = opt;
    uint16_t udp_len = static_cast<uint16_t>(opt - 34);
    uint16_t ip_len  = static_cast<uint16_t>(opt - 14);
    udp->len      = htons(udp_len);
    ip->tot_len   = htons(ip_len);
    return buf;
}

int main() {
    std::println("=== DHCP Engine Demo ===");

    // ── 1. Construct engine with a small address pool ────────────────────────
    Scalpel::Logic::DhcpEngine engine(
        "192.168.50.1",
        Scalpel::Logic::DhcpPoolConfig{
            Net::parse_ipv4("192.168.50.100"),
            Net::parse_ipv4("192.168.50.110"),
            std::chrono::seconds{3600}});

    std::println("[INFO] DhcpEngine initialised — router=192.168.50.1, "
                 "pool=192.168.50.100–192.168.50.110, lease=3600s");

    // ── 2. Build a DISCOVER packet and enqueue it ────────────────────────────
    size_t pkt_len = 0;
    auto   raw     = make_discover(pkt_len);
    auto   pkt     = Net::ParsedPacket::parse(std::span<uint8_t>{raw.data(), pkt_len});

    assert(pkt.is_valid_ipv4() && "DISCOVER frame must parse as valid IPv4");
    engine.intercept_request(pkt);
    std::println("[INFO] DISCOVER enqueued.");

    // ── 3. Use socketpair(AF_UNIX) as a loopback fd — no root needed ─────────
    int sv[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0 && "socketpair failed");

    engine.process_background_tasks(sv[1]);   // engine writes OFFER to sv[1]

    // ── 4. Read the OFFER back from sv[0] ────────────────────────────────────
    std::array<uint8_t, 512> response{};
    ssize_t n = recv(sv[0], response.data(), response.size(), MSG_DONTWAIT);
    assert(n > 0 && "Expected DHCP OFFER response");

    ::close(sv[0]);
    ::close(sv[1]);

    // ── 5. Minimal validation: check DHCP message type option = 2 (OFFER) ────
    // OFFER response layout: Ethernet(14) + IPv4(20) + UDP(8) + DhcpWireHdr(236)
    // Options start at byte 278.
    if (static_cast<size_t>(n) > 278 + sizeof(DhcpWireHdr)) {
        const uint8_t* opts = response.data() + 42 + sizeof(DhcpWireHdr);
        const uint8_t* end  = response.data() + static_cast<size_t>(n);
        while (opts < end && *opts != 255) {
            if (*opts == 0) { ++opts; continue; }
            uint8_t len = opts[1];
            if (opts[0] == 53 && len == 1) {
                assert(opts[2] == 2 && "DHCP message type must be OFFER (2)");
                std::println("[PASS] DHCP OFFER received (msg-type=2).");
                break;
            }
            opts += 2 + len;
        }
    }

    // ── 6. Reconfigure pool and verify engine accepts the new config ──────────
    if (auto rr = engine.reconfigure(Scalpel::Logic::DhcpPoolConfig{
            Net::parse_ipv4("10.0.0.100"),
            Net::parse_ipv4("10.0.0.200"),
            std::chrono::seconds{7200}});
        !rr) {
        std::println(stderr, "[FAIL] reconfigure: {}", rr.error());
        return 1;
    }
    std::println("[PASS] reconfigure() accepted new pool without crash.");

    std::println("=== Done ===");
    return 0;
}
