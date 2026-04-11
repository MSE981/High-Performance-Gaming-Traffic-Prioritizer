#include "SelfTest.hpp"
#include <memory>
#include <chrono>
#include <cstdio>
#include <print>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include "Config.hpp"
#include "NatEngine.hpp"
#include "DnsEngine.hpp"
#include "DhcpEngine.hpp"
#include "FirewallEngine.hpp"
#include "Processor.hpp"

namespace Scalpel::SelfTest {

// Wire-format DHCP header — mirrors the internal layout used by DhcpEngine.
// Defined locally here so DhcpEngine's private type stays confined to its TU.
#pragma pack(push, 1)
struct DhcpWireHeader {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
};
#pragma pack(pop)

// ── Worker ───────────────────────────────────────────────────────────────────

void SelfTest::run() {
    Report r;
    test_nat(r);
    test_dns(r);
    test_dhcp(r);
    test_firewall(r);
    test_classifier(r);
    test_system(r);
    if (callback_) callback_(r);  // §3.3.2: callback signals thread termination
}

// ── Packet builders ──────────────────────────────────────────────────────────
// All use std::array — zero heap allocation

std::array<uint8_t, 42> SelfTest::make_udp_pkt(Net::IPv4Net sip, Net::IPv4Net dip,
                                                 uint16_t sport, uint16_t dport) {
    std::array<uint8_t, 42> buf{};
    auto* eth  = reinterpret_cast<Net::EthernetHeader*>(buf.data());
    auto* ipv4 = reinterpret_cast<Net::IPv4Header*>(buf.data() + 14);
    auto* udp  = reinterpret_cast<Net::UDPHeader*>(buf.data() + 34);

    eth->proto       = htons(0x0800);
    ipv4->ver_ihl    = 0x45;
    ipv4->protocol   = 17;
    ipv4->tot_len    = htons(28); // 20 IP + 8 UDP
    ipv4->saddr      = sip;
    ipv4->daddr      = dip;
    udp->source      = htons(sport);
    udp->dest        = htons(dport);
    udp->len         = htons(8);
    return buf;
}

std::array<uint8_t, 54> SelfTest::make_tcp_pkt(Net::IPv4Net sip, Net::IPv4Net dip,
                                                 uint16_t sport, uint16_t dport,
                                                 uint16_t flags) {
    std::array<uint8_t, 54> buf{};
    auto* eth  = reinterpret_cast<Net::EthernetHeader*>(buf.data());
    auto* ipv4 = reinterpret_cast<Net::IPv4Header*>(buf.data() + 14);
    auto* tcp  = reinterpret_cast<Net::TCPHeader*>(buf.data() + 34);

    eth->proto           = htons(0x0800);
    ipv4->ver_ihl        = 0x45;
    ipv4->protocol       = 6;
    ipv4->tot_len        = htons(40); // 20 IP + 20 TCP
    ipv4->saddr          = sip;
    ipv4->daddr          = dip;
    tcp->source          = htons(sport);
    tcp->dest            = htons(dport);
    tcp->res1_doff_flags = htons(flags);
    return buf;
}

// DNS query: 42-byte UDP header + 12-byte DnsHeader + QNAME wire + QTYPE + QCLASS
std::array<uint8_t, 512> SelfTest::make_dns_query(Net::IPv4Net sip, Net::IPv4Net dip,
                                                    const char* host_dotted,
                                                    size_t& out_len) {
    std::array<uint8_t, 512> buf{};

    auto* eth  = reinterpret_cast<Net::EthernetHeader*>(buf.data());
    auto* ipv4 = reinterpret_cast<Net::IPv4Header*>(buf.data() + 14);
    auto* udp  = reinterpret_cast<Net::UDPHeader*>(buf.data() + 34);

    eth->proto     = htons(0x0800);
    ipv4->ver_ihl  = 0x45;
    ipv4->protocol = 17;
    ipv4->saddr    = sip;
    ipv4->daddr    = dip;
    udp->source    = htons(12345);
    udp->dest      = htons(53);

    size_t dns_off = 42;
    buf[dns_off + 0] = 0x12; buf[dns_off + 1] = 0x34; // id
    buf[dns_off + 2] = 0x01; buf[dns_off + 3] = 0x00; // flags: standard query RD=1
    buf[dns_off + 4] = 0x00; buf[dns_off + 5] = 0x01; // qdcount = 1

    size_t qname_off = dns_off + 12; // 12 = sizeof(DnsHeader)
    size_t pos = qname_off;
    const char* p = host_dotted;
    while (*p && pos < 500) {
        size_t lstart = pos++;
        uint8_t llen = 0;
        while (*p && *p != '.' && pos < 510) {
            buf[pos++] = static_cast<uint8_t>(*p++);
            ++llen;
        }
        buf[lstart] = llen;
        if (*p == '.') ++p;
    }
    buf[pos++] = 0;           // root label
    buf[pos++] = 0x00; buf[pos++] = 0x01; // QTYPE A
    buf[pos++] = 0x00; buf[pos++] = 0x01; // QCLASS IN

    out_len = pos;
    uint16_t udp_payload = static_cast<uint16_t>(pos - 34);
    uint16_t ip_payload  = static_cast<uint16_t>(pos - 14);
    udp->len         = htons(udp_payload);
    ipv4->tot_len    = htons(ip_payload);
    return buf;
}

// DHCP DISCOVER: 42-byte UDP (src=68, dst=67) + DhcpWireHeader + options
std::array<uint8_t, 512> SelfTest::make_dhcp_discover(size_t& out_len) {
    std::array<uint8_t, 512> buf{};
    auto* eth  = reinterpret_cast<Net::EthernetHeader*>(buf.data());
    auto* ipv4 = reinterpret_cast<Net::IPv4Header*>(buf.data() + 14);
    auto* udp  = reinterpret_cast<Net::UDPHeader*>(buf.data() + 34);

    eth->proto     = htons(0x0800);
    ipv4->ver_ihl  = 0x45;
    ipv4->protocol = 17;
    ipv4->saddr    = Net::IPv4Net{};             // 0.0.0.0 (DISCOVER)
    ipv4->daddr    = Net::IPv4Net{0xFFFFFFFF};   // broadcast

    udp->source = htons(68);
    udp->dest   = htons(67);

    auto* dhcp = reinterpret_cast<DhcpWireHeader*>(buf.data() + 42);
    dhcp->op           = 1;  // BootRequest
    dhcp->htype        = 1;  // Ethernet
    dhcp->hlen         = 6;
    dhcp->xid          = htonl(0xDEADBEEF);
    dhcp->magic_cookie = htonl(0x63825363);
    // Synthetic client MAC
    dhcp->chaddr[0] = 0xAA; dhcp->chaddr[1] = 0xBB; dhcp->chaddr[2] = 0xCC;
    dhcp->chaddr[3] = 0xDD; dhcp->chaddr[4] = 0xEE; dhcp->chaddr[5] = 0xFF;

    size_t opt = 42 + sizeof(DhcpWireHeader);
    buf[opt++] = 53; buf[opt++] = 1; buf[opt++] = 1; // DHCP Message Type = DISCOVER
    buf[opt++] = 255; // End option

    out_len = opt;
    uint16_t udp_len = static_cast<uint16_t>(opt - 34);
    uint16_t ip_len  = static_cast<uint16_t>(opt - 14);
    udp->len      = htons(udp_len);
    ipv4->tot_len = htons(ip_len);
    return buf;
}

// ── Sub-tests ────────────────────────────────────────────────────────────────

void SelfTest::test_nat(Report& r) {
    // make_unique: NatEngine has ~1.3 MB internal arrays — too large for stack
    auto nat = std::make_unique<Logic::NatEngine>();
    Net::IPv4Net wan_ip = Config::parse_ip_str("10.0.0.1");
    nat->set_wan_ip(wan_ip);

    Net::IPv4Net lan_ip = Config::parse_ip_str("192.168.1.100");
    Net::IPv4Net ext_ip = Config::parse_ip_str("8.8.8.8");
    auto buf = make_udp_pkt(lan_ip, ext_ip, 54321, 12345);
    auto pkt = Net::ParsedPacket::parse(std::span<uint8_t>{buf.data(), 42});

    bool ok = nat->process_outbound(pkt);

    bool snat_pass = ok && pkt.is_valid_ipv4() && (pkt.ipv4->saddr == wan_ip);
    r.add("NAT_SNAT", snat_pass,
          snat_pass ? "saddr rewritten to WAN IP" : "saddr not rewritten");

    auto* udp = pkt.udp();
    uint16_t ext_port = udp ? ntohs(udp->source) : 0;
    bool session_pass = (ext_port >= 10000 && ext_port <= 60000);
    r.add("NAT_Session", session_pass,
          session_pass ? "ephemeral port in valid range" : "port out of range");
}

void SelfTest::test_dns(Report& r) {
    // Save and restore global static DNS count to avoid side-effects on live engine
    size_t saved_count = Config::STATIC_DNS_COUNT;

    // ── DNS_Static_Record ──
    Config::upsert_static_dns("test.local", "1.2.3.4");
    auto dns_static = std::make_unique<Logic::DnsEngine>();
    dns_static->reload_static_records();

    size_t pkt_len = 0;
    Net::IPv4Net cli = Config::parse_ip_str("192.168.1.100");
    Net::IPv4Net srv = Config::parse_ip_str("8.8.8.8");
    auto dns_buf = make_dns_query(cli, srv, "test.local", pkt_len);
    auto pkt_static = Net::ParsedPacket::parse(
        std::span<uint8_t>{dns_buf.data(), pkt_len});
    // bounce_fd=-1: send() will fail (EBADF), increments dropped counter by 1 — acceptable
    bool static_pass = dns_static->process_query(pkt_static, -1);
    r.add("DNS_Static", static_pass,
          static_pass ? "static record hit and bounced" : "static record lookup failed");

    Config::STATIC_DNS_COUNT = saved_count; // restore global state

    // ── DNS_Cache_Miss ──
    auto dns_miss = std::make_unique<Logic::DnsEngine>();
    size_t len2 = 0;
    auto miss_buf = make_dns_query(cli, srv, "unknown.invalid", len2);
    auto pkt_miss = Net::ParsedPacket::parse(
        std::span<uint8_t>{miss_buf.data(), len2});
    bool miss_pass = !dns_miss->process_query(pkt_miss, -1);
    r.add("DNS_CacheMiss", miss_pass,
          miss_pass ? "unknown domain returns false (cache miss)" : "unexpected cache hit");

    // ── DNS_Redirect ──
    auto dns_redir = std::make_unique<Logic::DnsEngine>();
    Net::IPv4Net upstream = Config::parse_ip_str("9.9.9.9");
    dns_redir->set_upstream({upstream, Net::IPv4Net{}});
    dns_redir->set_redirect(true);
    size_t len3 = 0;
    auto redir_buf = make_dns_query(cli, srv, "redirect.test", len3);
    auto pkt_redir = Net::ParsedPacket::parse(
        std::span<uint8_t>{redir_buf.data(), len3});
    dns_redir->process_query(pkt_redir, -1); // cache miss → rewrites daddr
    bool redir_pass = pkt_redir.is_valid_ipv4() && (pkt_redir.ipv4->daddr == upstream);
    r.add("DNS_Redirect", redir_pass,
          redir_pass ? "daddr redirected to upstream" : "daddr not rewritten");
}

void SelfTest::test_dhcp(Report& r) {
    Logic::DhcpEngine dhcp(
        "192.168.1.1",
        Logic::DhcpPoolConfig{
            Net::parse_ipv4("192.168.1.100"),
            Net::parse_ipv4("192.168.1.200"),
            std::chrono::seconds{86400}});

    size_t pkt_len = 0;
    auto dhcp_buf = make_dhcp_discover(pkt_len);
    auto pkt = Net::ParsedPacket::parse(
        std::span<uint8_t>{dhcp_buf.data(), pkt_len});

    dhcp.intercept_request(pkt); // enqueue into request_queue

    // Use socketpair(AF_UNIX, SOCK_DGRAM) — no root required.
    // DhcpEngine::handle_dhcp_request calls send(lan_fd, response, len, MSG_DONTWAIT).
    // On a SOCK_DGRAM unix socket, this succeeds. We read from the other end to verify.
    int sv[2] = {-1, -1};
    bool dhcp_pass = false;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
        dhcp.process_background_tasks(sv[1]);
        uint8_t probe[1];
        // Non-blocking check: §3.3.4 — no sleep; use MSG_DONTWAIT
        ssize_t n = recv(sv[0], probe, 1, MSG_DONTWAIT);
        dhcp_pass = (n > 0);
        ::close(sv[0]);
        ::close(sv[1]);
    }
    r.add("DHCP_Queue", dhcp_pass,
          dhcp_pass ? "DISCOVER processed, OFFER sent" : "no OFFER response received");
}

void SelfTest::test_firewall(Report& r) {
    // make_unique: FirewallEngine has ~5 MB table — must be heap-allocated
    size_t saved_policy_count = Config::DEVICE_POLICY_COUNT;

    // ── FW_Block ──
    auto fw_block = std::make_unique<Logic::FirewallEngine>();
    Net::IPv4Net block_ip = Config::parse_ip_str("192.168.1.200");
    Config::DevicePolicy p{};
    p.ip      = block_ip;
    p.blocked = true;
    // mac stays zero-initialized: firewall block logic only checks ip
    Config::upsert_device_policy(p);
    fw_block->sync_blocked_ips();
    bool block_pass = fw_block->is_blocked_ip(block_ip);
    r.add("FW_Block", block_pass,
          block_pass ? "blocked IP correctly rejected" : "is_blocked_ip returned false");

    Config::DEVICE_POLICY_COUNT = saved_policy_count; // restore global state

    // ── FW_Session ──
    auto fw_sess = std::make_unique<Logic::FirewallEngine>();
    Net::IPv4Net lan_ip = Config::parse_ip_str("192.168.1.100");
    Net::IPv4Net srv_ip = Config::parse_ip_str("1.2.3.4");

    // Outbound SYN: flags = 0x5002 (data_offset=5, SYN=1)
    auto syn_buf = make_tcp_pkt(lan_ip, srv_ip, 54321, 80, 0x5002);
    auto syn_pkt = Net::ParsedPacket::parse(
        std::span<uint8_t>{syn_buf.data(), 54});
    fw_sess->track_outbound(syn_pkt);

    // Inbound SYN-ACK: flags = 0x5012 (data_offset=5, SYN=1, ACK=1)
    auto ack_buf = make_tcp_pkt(srv_ip, lan_ip, 80, 54321, 0x5012);
    auto ack_pkt = Net::ParsedPacket::parse(
        std::span<uint8_t>{ack_buf.data(), 54});
    bool session_pass = fw_sess->check_inbound(ack_pkt);
    r.add("FW_Session", session_pass,
          session_pass ? "SYN-ACK allowed by conntrack" : "SYN-ACK unexpectedly blocked");
}

void SelfTest::test_classifier(Report& r) {
    Net::IPv4Net src = Config::parse_ip_str("192.168.1.100");
    Net::IPv4Net dst = Config::parse_ip_str("8.8.8.8");

    // ── PRIO_DNS: UDP dst=53 → Critical ──
    {
        Logic::HeuristicProcessor proc;
        auto buf = make_udp_pkt(src, dst, 40000, 53);
        auto pkt = Net::ParsedPacket::parse(std::span<uint8_t>{buf.data(), 42});
        bool pass = (proc.process(pkt) == Net::Priority::Critical);
        r.add("PRIO_DNS", pass,
              pass ? "UDP/53 classified Critical" : "wrong priority for DNS");
    }

    // ── PRIO_Gaming: small UDP dst=3074 → High ──
    {
        Logic::HeuristicProcessor proc;
        auto buf = make_udp_pkt(src, dst, 40000, 3074);
        auto pkt = Net::ParsedPacket::parse(std::span<uint8_t>{buf.data(), 42});
        bool pass = (proc.process(pkt) == Net::Priority::High);
        r.add("PRIO_Gaming", pass,
              pass ? "UDP/3074 classified High" : "wrong priority for game port");
    }

    // ── PRIO_Normal: 1200-byte UDP to high port → Normal ──
    // sport=40000 avoids game port ranges {3074,27015,12000-12999}
    {
        Logic::HeuristicProcessor proc;
        std::array<uint8_t, 1200> big{};
        auto hdr = make_udp_pkt(src, dst, 40000, 50000);
        std::memcpy(big.data(), hdr.data(), 42);
        auto* ipv4 = reinterpret_cast<Net::IPv4Header*>(big.data() + 14);
        auto* udp  = reinterpret_cast<Net::UDPHeader*>(big.data() + 34);
        ipv4->tot_len = htons(static_cast<uint16_t>(1200 - 14));
        udp->len      = htons(static_cast<uint16_t>(1200 - 14 - 20));
        auto pkt = Net::ParsedPacket::parse(std::span<uint8_t>{big.data(), 1200});
        bool pass = (proc.process(pkt) == Net::Priority::Normal);
        r.add("PRIO_Normal", pass,
              pass ? "1200B UDP classified Normal" : "wrong priority for bulk traffic");
    }
}

// §2.3.7: Hardware checks via raw fd — no ifstream, no heap
void SelfTest::test_system(Report& r) {
    // SYS_Temp: /sys/class/thermal/thermal_zone0/temp
    {
        char buf[16]{};
        int fd = ::open("/sys/class/thermal/thermal_zone0/temp", O_RDONLY);
        bool pass = false;
        if (fd >= 0) {
            ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
            ::close(fd);
            if (n > 0) { buf[n] = '\0'; pass = (atof(buf) > 0.0); }
        }
        r.add("SYS_Temp", pass,
              pass ? "CPU thermal sensor readable" : "cannot read /sys/class/thermal");
    }

    // SYS_Memory: /proc/meminfo, scan for MemTotal
    {
        char mbuf[512]{};
        bool pass = false;
        int mfd = ::open("/proc/meminfo", O_RDONLY);
        if (mfd >= 0) {
            ssize_t n = ::read(mfd, mbuf, sizeof(mbuf) - 1);
            ::close(mfd);
            if (n > 0) {
                const char* mt = strstr(mbuf, "MemTotal:");
                if (mt) {
                    unsigned long total = 0;
                    sscanf(mt, "MemTotal: %lu", &total);
                    pass = (total > 0);
                }
            }
        }
        r.add("SYS_Memory", pass,
              pass ? "MemTotal parsed from /proc/meminfo" : "cannot read memory info");
    }

    // SYS_Ifaces: /sys/class/net — count non-lo interfaces
    {
        uint8_t iface_count = 0;
        DIR* d = opendir("/sys/class/net");
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != nullptr) {
                if (de->d_name[0] == '.') continue;
                if (strncmp(de->d_name, "lo", 3) == 0) continue;
                ++iface_count;
            }
            closedir(d);
        }
        bool pass = (iface_count > 0);
        r.add("SYS_Ifaces", pass,
              pass ? "network interfaces detected" : "no network interfaces found");
    }
}

} // namespace Scalpel::SelfTest
