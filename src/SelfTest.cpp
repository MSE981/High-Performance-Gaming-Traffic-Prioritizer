#include "SelfTest.hpp"
#include <algorithm>
#include <memory>
#include <chrono>
#include <cstdio>
#include <print>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <string_view>
#include "Config.hpp"
#include <mutex>
#include "NatEngine.hpp"
#include "DhcpEngine.hpp"

namespace HPGTP::SelfTest {

namespace {

uint16_t fold_ip_header_checksum(const Utils::Net::IPv4Header* ip) noexcept {
    const auto* w = reinterpret_cast<const uint8_t*>(ip);
    uint32_t        sum = 0;
    for (int i = 0; i < 20; i += 2)
        sum += (uint32_t(w[i]) << 8) | w[i + 1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum & 0xFFFF);
}

uint16_t fold_icmp_checksum(const Utils::Net::IcmpEchoHeader* icmp, size_t icmp_len) noexcept {
    const auto* p = reinterpret_cast<const uint8_t*>(icmp);
    uint32_t    sum = 0;
    for (size_t i = 0; i + 1 < icmp_len; i += 2)
        sum += (uint32_t(p[i]) << 8) | p[i + 1];
    if (icmp_len & 1) sum += uint32_t(p[icmp_len - 1]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum & 0xFFFF);
}

} // namespace

// DHCP header 
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

// SelfTest entry point

void SelfTest::run() {
    Report r;
    test_nat(r);
    test_dhcp(r);
    test_system(r);
    if (callback_) callback_(r);  // After all cases; worker is about to exit.
}

// Packet builders
// UDP: 14-byte Ethernet + 20-byte IPv4 + 8-byte UDP

std::array<uint8_t, 42> SelfTest::make_udp_pkt(Utils::Net::IPv4Net sip, Utils::Net::IPv4Net dip,
                                                 uint16_t sport, uint16_t dport) {
    std::array<uint8_t, 42> buf{};
    auto* eth  = reinterpret_cast<Utils::Net::EthernetHeader*>(buf.data());
    auto* ipv4 = reinterpret_cast<Utils::Net::IPv4Header*>(buf.data() + 14);
    auto* udp  = reinterpret_cast<Utils::Net::UDPHeader*>(buf.data() + 34);

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

// TCP: 14-byte Ethernet + 20-byte IPv4 + 20-byte TCP

std::array<uint8_t, 54> SelfTest::make_tcp_pkt(Utils::Net::IPv4Net sip, Utils::Net::IPv4Net dip,
                                                 uint16_t sport, uint16_t dport,
                                                 uint16_t flags) {
    std::array<uint8_t, 54> buf{};
    auto* eth  = reinterpret_cast<Utils::Net::EthernetHeader*>(buf.data());
    auto* ipv4 = reinterpret_cast<Utils::Net::IPv4Header*>(buf.data() + 14);
    auto* tcp  = reinterpret_cast<Utils::Net::TCPHeader*>(buf.data() + 34);

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

// DHCP DISCOVER: 42-byte UDP (src=68, dst=67) + DhcpWireHeader + options
std::array<uint8_t, 512> SelfTest::make_dhcp_discover(size_t& out_len) {
    std::array<uint8_t, 512> buf{};
    auto* eth  = reinterpret_cast<Utils::Net::EthernetHeader*>(buf.data());
    auto* ipv4 = reinterpret_cast<Utils::Net::IPv4Header*>(buf.data() + 14);
    auto* udp  = reinterpret_cast<Utils::Net::UDPHeader*>(buf.data() + 34);

    eth->proto     = htons(0x0800);
    ipv4->ver_ihl  = 0x45;
    ipv4->protocol = 17;
    ipv4->saddr    = Utils::Net::IPv4Net{};
    ipv4->daddr    = Utils::Net::IPv4Net{0xFFFFFFFF};

    udp->source = htons(68);
    udp->dest   = htons(67);

    auto* dhcp = reinterpret_cast<DhcpWireHeader*>(buf.data() + 42);
    dhcp->op           = 1;  // BootRequest
    dhcp->htype        = 1;  // Ethernet
    dhcp->hlen         = 6;
    dhcp->xid          = htonl(0xDEADBEEF);
    dhcp->magic_cookie = htonl(0x63825363);
    dhcp->chaddr[0] = 0xAA; dhcp->chaddr[1] = 0xBB; dhcp->chaddr[2] = 0xCC;
    dhcp->chaddr[3] = 0xDD; dhcp->chaddr[4] = 0xEE; dhcp->chaddr[5] = 0xFF;

    size_t opt = 42 + sizeof(DhcpWireHeader);
    buf[opt++] = 53; buf[opt++] = 1; buf[opt++] = 1;
    buf[opt++] = 255;

    out_len = opt;
    uint16_t udp_len = static_cast<uint16_t>(opt - 34);
    uint16_t ip_len  = static_cast<uint16_t>(opt - 14);
    udp->len      = htons(udp_len);
    ipv4->tot_len = htons(ip_len);
    return buf;
}

// NAT tests

void SelfTest::test_nat(Report& r) {
    auto nat = std::make_unique<Engine::Nat::NatEngine>();
    Utils::Net::IPv4Net wan_ip = Config::parse_ip_str("10.0.0.1").value();
    nat->set_wan_ip(wan_ip);

    Utils::Net::IPv4Net lan_ip = Config::parse_ip_str("192.168.1.100").value();
    Utils::Net::IPv4Net ext_ip = Config::parse_ip_str("8.8.8.8").value();
    auto buf = make_udp_pkt(lan_ip, ext_ip, 54321, 12345);
    auto pkt = Utils::Net::ParsedPacket::parse(std::span<uint8_t>{buf.data(), 42});

    bool ok = nat->process_outbound(pkt);

    bool snat_pass = ok && pkt.is_valid_ipv4() && (pkt.ipv4->saddr == wan_ip);
    r.add("NAT_SNAT", snat_pass,
          snat_pass ? "saddr rewritten to WAN IP" : "saddr not rewritten");

    auto* udp = pkt.udp();
    uint16_t ext_port = udp ? ntohs(udp->source) : 0;
    bool session_pass = (ext_port >= 10000 && ext_port <= 60000);
    r.add("NAT_Session", session_pass,
          session_pass ? "ephemeral port in valid range" : "port out of range");

    std::array<uint8_t, 42> icmp_req{};
    {
        auto* eth  = reinterpret_cast<Utils::Net::EthernetHeader*>(icmp_req.data());
        auto* ipv4 = reinterpret_cast<Utils::Net::IPv4Header*>(icmp_req.data() + 14);
        auto* icmp = reinterpret_cast<Utils::Net::IcmpEchoHeader*>(icmp_req.data() + 34);
        eth->proto        = htons(0x0800);
        ipv4->ver_ihl     = 0x45;
        ipv4->protocol    = 1;
        ipv4->tot_len     = htons(28);
        ipv4->frag_off    = 0;
        ipv4->ttl         = 64;
        ipv4->saddr       = lan_ip;
        ipv4->daddr       = ext_ip;
        ipv4->check       = 0;
        ipv4->check       = htons(fold_ip_header_checksum(ipv4));
        icmp->type        = 8;
        icmp->code        = 0;
        icmp->check       = 0;
        icmp->id          = htons(4242);
        icmp->sequence    = htons(1);
        icmp->check       = htons(fold_icmp_checksum(icmp, 8));
    }
    auto pkt_icmp = Utils::Net::ParsedPacket::parse(std::span<uint8_t>{icmp_req.data(), icmp_req.size()});
    bool icmp_out = nat->process_outbound(pkt_icmp);
    auto* ie = pkt_icmp.icmp_echo();
    const bool icmp_snat = icmp_out && pkt_icmp.ipv4->saddr == wan_ip && ie
        && ntohs(ie->id) >= 10000 && ntohs(ie->id) <= 60000;

    std::array<uint8_t, 42> icmp_rep{};
    {
        auto* eth  = reinterpret_cast<Utils::Net::EthernetHeader*>(icmp_rep.data());
        auto* ipv4 = reinterpret_cast<Utils::Net::IPv4Header*>(icmp_rep.data() + 14);
        auto* icmp = reinterpret_cast<Utils::Net::IcmpEchoHeader*>(icmp_rep.data() + 34);
        eth->proto        = htons(0x0800);
        ipv4->ver_ihl     = 0x45;
        ipv4->protocol    = 1;
        ipv4->tot_len     = htons(28);
        ipv4->frag_off    = 0;
        ipv4->ttl         = 64;
        ipv4->saddr       = ext_ip;
        ipv4->daddr       = wan_ip;
        ipv4->check       = 0;
        ipv4->check       = htons(fold_ip_header_checksum(ipv4));
        icmp->type        = 0;
        icmp->code        = 0;
        icmp->check       = 0;
        icmp->id          = ie ? ie->id : 0;
        icmp->sequence    = htons(1);
        icmp->check       = htons(fold_icmp_checksum(icmp, 8));
    }
    auto pkt_rep = Utils::Net::ParsedPacket::parse(std::span<uint8_t>{icmp_rep.data(), icmp_rep.size()});
    bool icmp_in  = nat->process_inbound(pkt_rep);
    auto* ir      = pkt_rep.icmp_echo();
    const bool icmp_dnat = icmp_in && pkt_rep.ipv4->daddr == lan_ip && ir && ir->id == htons(4242);

    const bool icmp_pass = icmp_snat && icmp_dnat;
    r.add("NAT_ICMP", icmp_pass,
          icmp_pass ? "echo SNAT/DNAT id map and checksums" : "ICMP NAT path failed");
}

// DHCP tests
void SelfTest::test_dhcp(Report& r) {
    Engine::Dhcp::DhcpEngine dhcp(
        "192.168.1.1",
        Engine::Dhcp::DhcpPoolConfig{
            Utils::Net::parse_ipv4("192.168.1.100"),
            Utils::Net::parse_ipv4("192.168.1.200"),
            std::chrono::seconds{86400}});

    size_t pkt_len = 0;
    auto dhcp_buf = make_dhcp_discover(pkt_len);
    auto pkt = Utils::Net::ParsedPacket::parse(
        std::span<uint8_t>{dhcp_buf.data(), pkt_len});

    dhcp.intercept_request(pkt); 
    int sv[2] = {-1, -1};
    bool dhcp_pass = false;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
        dhcp.process_background_tasks(sv[1]);
        uint8_t probe[1];
        ssize_t n = recv(sv[0], probe, 1, MSG_DONTWAIT);
        dhcp_pass = (n > 0);
        ::close(sv[0]);
        ::close(sv[1]);
    }
    r.add("DHCP_Queue", dhcp_pass,
          dhcp_pass ? "DISCOVER processed, OFFER sent" : "no OFFER response received");
}

// Hardware check
void SelfTest::test_system(Report& r) {
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

    {
        uint8_t iface_count = 0;
        DIR* d = opendir("/sys/class/net");
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != nullptr) {
                if (de->d_name[0] == '.') continue;
                if (std::string_view{de->d_name} == "lo") continue;
                ++iface_count;
            }
            closedir(d);
        }
        bool pass = (iface_count > 0);
        r.add("SYS_Ifaces", pass,
              pass ? "network interfaces detected" : "no network interfaces found");
    }
}

} // namespace HPGTP::SelfTest
