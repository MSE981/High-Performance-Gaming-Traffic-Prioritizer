#include "DhcpEngine.hpp"
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <print>
#include <algorithm>

namespace Scalpel::Logic {

// DhcpHeader is an internal wire-format struct — hidden from all clients
#pragma pack(push, 1)
struct DhcpHeader {
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

// ── private helpers ──────────────────────────────────────────────────────────

void DhcpEngine::init_pool(Net::IPv4Net start_ip, Net::IPv4Net end_ip) {
    uint32_t start_h = start_ip.to_host().raw();
    uint32_t end_h   = end_ip.to_host().raw();
    if (end_h < start_h) end_h = start_h;
    pool_count = std::min(static_cast<size_t>(end_h - start_h + 1), MAX_POOL_SIZE);
    for (size_t i = 0; i < pool_count; ++i) {
        leases[i].ip     = Net::pool_advance(start_ip, static_cast<uint32_t>(i));
        leases[i].active = false;
    }
}

Net::IPv4Net DhcpEngine::find_or_assign_lease(const uint8_t* mac) {
    auto now      = std::chrono::steady_clock::now();
    auto duration = lease_duration;

    for (size_t i = 0; i < pool_count; ++i)
        if (leases[i].active && std::memcmp(leases[i].mac, mac, 6) == 0) return leases[i].ip;

    for (size_t i = 0; i < pool_count; ++i) {
        if (!leases[i].active || now > leases[i].lease_expiry) {
            std::memcpy(leases[i].mac, mac, 6);
            leases[i].active       = true;
            leases[i].lease_expiry = now + duration;
            return leases[i].ip;
        }
    }
    return Net::IPv4Net{};
}

void DhcpEngine::commit_lease(const uint8_t* mac, Net::IPv4Net ip) {
    auto duration = lease_duration;
    for (size_t i = 0; i < pool_count; ++i) {
        if (leases[i].ip == ip) {
            leases[i].active       = true;
            leases[i].lease_expiry = std::chrono::steady_clock::now() + duration;
            return;
        }
    }
}

void DhcpEngine::handle_dhcp_request(DhcpMessage& msg, int lan_fd) {
    auto parsed = Net::ParsedPacket::parse(std::span<uint8_t>(msg.data.data(), msg.len));
    if (!parsed.is_valid_ipv4()) return;
    if (parsed.l4_protocol != 17) return;

    auto udp = parsed.udp();
    if (!udp || ntohs(udp->dest) != 67) return;

    size_t dhcp_offset = parsed.l4_offset + sizeof(Net::UDPHeader);
    if (msg.len < dhcp_offset + sizeof(DhcpHeader)) return;

    auto dhcp = reinterpret_cast<const DhcpHeader*>(msg.data.data() + dhcp_offset);
    if (dhcp->op != 1) return;
    if (ntohl(dhcp->magic_cookie) != 0x63825363) return;

    uint8_t msg_type = 0;
    const uint8_t* opt = msg.data.data() + dhcp_offset + sizeof(DhcpHeader);
    const uint8_t* end = msg.data.data() + msg.len;
    uint32_t raw_requested_ip = 0;

    while (opt < end && *opt != 255) {
        if (*opt == 0) { opt++; continue; }
        uint8_t len = opt[1];
        if (opt + 2 + len > end) break;
        if (opt[0] == 53 && len == 1) msg_type = opt[2];
        if (opt[0] == 50 && len == 4) std::memcpy(&raw_requested_ip, &opt[2], 4);
        opt += 2 + len;
    }
    Net::IPv4Net requested_ip{raw_requested_ip};

    // Build and send response — all wire construction happens below
    auto send_response = [&](uint8_t type, Net::IPv4Net yiaddr) {
        alignas(64) std::array<uint8_t, 512> response{};

        auto eth = reinterpret_cast<Net::EthernetHeader*>(response.data());
        std::memcpy(eth->dest, dhcp->chaddr, 6);
        std::memcpy(eth->src, parsed.eth->dest, 6);
        eth->proto = htons(0x0800);

        auto ip = reinterpret_cast<Net::IPv4Header*>(response.data() + sizeof(Net::EthernetHeader));
        ip->ver_ihl  = 0x45;
        ip->tos      = 0;
        ip->id       = 0;
        ip->frag_off = 0;
        ip->ttl      = 64;
        ip->protocol = 17;
        ip->saddr    = router_ip;
        ip->daddr    = Net::IPv4Net{0xFFFFFFFF};

        auto resp_udp = reinterpret_cast<Net::UDPHeader*>(
            response.data() + sizeof(Net::EthernetHeader) + sizeof(Net::IPv4Header));
        resp_udp->source = htons(67);
        resp_udp->dest   = htons(68);

        auto resp_dhcp = reinterpret_cast<DhcpHeader*>(
            response.data() + sizeof(Net::EthernetHeader) + sizeof(Net::IPv4Header) + sizeof(Net::UDPHeader));
        resp_dhcp->op           = 2;
        resp_dhcp->htype        = 1;
        resp_dhcp->hlen         = 6;
        resp_dhcp->hops         = 0;
        resp_dhcp->xid          = dhcp->xid;
        resp_dhcp->secs         = 0;
        resp_dhcp->flags        = htons(0x8000);
        resp_dhcp->ciaddr       = 0;
        resp_dhcp->yiaddr       = yiaddr.raw();
        resp_dhcp->siaddr       = router_ip.raw();
        resp_dhcp->giaddr       = 0;
        std::memcpy(resp_dhcp->chaddr, dhcp->chaddr, 16);
        resp_dhcp->magic_cookie = htonl(0x63825363);

        uint8_t* o = response.data() + sizeof(Net::EthernetHeader) + sizeof(Net::IPv4Header)
                     + sizeof(Net::UDPHeader) + sizeof(DhcpHeader);
        *o++ = 53; *o++ = 1; *o++ = type;
        *o++ = 1;  *o++ = 4; *o++ = 255; *o++ = 255; *o++ = 255; *o++ = 0;
        { uint32_t r = router_ip.raw(); *o++ = 3; *o++ = 4; std::memcpy(o, &r, 4); o += 4; }
        { uint32_t r = router_ip.raw(); *o++ = 6; *o++ = 4; std::memcpy(o, &r, 4); o += 4; }
        { uint32_t lt = htonl(static_cast<uint32_t>(lease_duration.count())); *o++ = 51; *o++ = 4; std::memcpy(o, &lt, 4); o += 4; }
        { uint32_t r = router_ip.raw(); *o++ = 54; *o++ = 4; std::memcpy(o, &r, 4); o += 4; }
        *o++ = 255;

        size_t dhcp_len  = static_cast<size_t>(o - reinterpret_cast<uint8_t*>(resp_dhcp));
        size_t udp_len   = sizeof(Net::UDPHeader) + dhcp_len;
        size_t ip_len    = sizeof(Net::IPv4Header) + udp_len;
        size_t total_len = sizeof(Net::EthernetHeader) + ip_len;

        ip->tot_len = htons(ip_len);
        ip->check   = 0;
        uint32_t ip_sum = 0;
        const uint16_t* ip_words = reinterpret_cast<const uint16_t*>(ip);
        for (size_t i = 0; i < sizeof(Net::IPv4Header)/2; ++i) ip_sum += ntohs(ip_words[i]);
        ip_sum = (ip_sum >> 16) + (ip_sum & 0xFFFF);
        ip_sum += (ip_sum >> 16);
        ip->check = htons(~ip_sum);

        resp_udp->len   = htons(udp_len);
        resp_udp->check = 0;

        send(lan_fd, response.data(), total_len, MSG_DONTWAIT);
    };

    if (msg_type == 1) { // DHCP Discover
        Net::IPv4Net offered_ip = find_or_assign_lease(dhcp->chaddr);
        if (offered_ip.raw() != 0) send_response(2, offered_ip);
    } else if (msg_type == 3) { // DHCP Request
        if (requested_ip.raw() == 0) requested_ip = Net::IPv4Net{dhcp->ciaddr};
        Net::IPv4Net leased_ip = find_or_assign_lease(dhcp->chaddr);

        if (leased_ip == requested_ip) {
            commit_lease(dhcp->chaddr, leased_ip);
            send_response(5, leased_ip);
            char ip_buf[INET_ADDRSTRLEN]{};
            uint32_t raw_ip = leased_ip.raw();
            inet_ntop(AF_INET, &raw_ip, ip_buf, sizeof(ip_buf));
            std::println("[DHCP Engine] Assigned IP to device: {}", ip_buf);
        } else {
            send_response(6, requested_ip);
        }
    }
}

// ── public API ────────────────────────────────────────────────────────────────

DhcpEngine::DhcpEngine(const std::string& lan_ip, DhcpPoolConfig cfg) {
    router_ip      = Net::parse_ipv4(lan_ip.c_str());
    lease_duration = cfg.lease;
    init_pool(cfg.pool_start, cfg.pool_end);
}

void DhcpEngine::reconfigure(DhcpPoolConfig cfg) {
    lease_duration = cfg.lease;
    init_pool(cfg.pool_start, cfg.pool_end);
}

void DhcpEngine::intercept_request(const Net::ParsedPacket& pkt) {
    DhcpMessage msg;
    msg.len = std::min(pkt.raw_span.size(), size_t(512));
    std::memcpy(msg.data.data(), pkt.raw_span.data(), msg.len);
    request_queue.push(msg);
}

void DhcpEngine::process_background_tasks(int lan_fd) {
    DhcpMessage msg;
    while (request_queue.pop(msg))
        handle_dhcp_request(msg, lan_fd);
}

} // namespace Scalpel::Logic
