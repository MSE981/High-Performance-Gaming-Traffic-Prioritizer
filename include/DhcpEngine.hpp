#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <span>
#include <print>
#include <arpa/inet.h>
#include "Headers.hpp"
#include "Config.hpp"

namespace Scalpel::Logic {

    struct DhcpMessage {
        size_t len = 0;
        std::array<uint8_t, 512> data{};
    };

#pragma pack(push, 1)
    struct DhcpHeader {
        uint8_t op;      // 1=BootRequest, 2=BootReply
        uint8_t htype;   // 1=Ethernet
        uint8_t hlen;    // 6=MAC length
        uint8_t hops;
        uint32_t xid;
        uint16_t secs;
        uint16_t flags;
        uint32_t ciaddr; // Client IP
        uint32_t yiaddr; // Your IP
        uint32_t siaddr; // Server IP
        uint32_t giaddr; // Gateway IP
        uint8_t chaddr[16]; // Client MAC
        uint8_t sname[64];
        uint8_t file[128];
        uint32_t magic_cookie; // 0x63825363
        // options follow
    };
#pragma pack(pop)

    struct DhcpLease {
        uint8_t      mac[6]{};
        Net::IPv4Net ip{};    // NBO — from pool, never host-order arithmetic
        bool         active = false;
        std::chrono::steady_clock::time_point lease_expiry;
    };

    // Phase 2.5: Zero-Allocation user-space DHCP server (no Dnsmasq needed)
    class DhcpEngine {
        Net::SpscRingBuffer<DhcpMessage, 512> request_queue{};

        static constexpr size_t MAX_POOL_SIZE = 253; // max usable IPs in a /24
        std::array<DhcpLease, MAX_POOL_SIZE> leases{};
        size_t       pool_count   = 0;
        Net::IPv4Net router_ip{};
        uint32_t     lease_seconds = 86400;

        void init_pool(Net::IPv4Net start_ip, Net::IPv4Net end_ip) {
            uint32_t start_h = start_ip.to_host().raw();
            uint32_t end_h   = end_ip.to_host().raw();
            if (end_h < start_h) end_h = start_h;
            pool_count = std::min(static_cast<size_t>(end_h - start_h + 1), MAX_POOL_SIZE);
            for (size_t i = 0; i < pool_count; ++i) {
                leases[i].ip     = Net::pool_advance(start_ip, static_cast<uint32_t>(i));
                leases[i].active = false;
            }
        }

    public:
        DhcpEngine(const std::string& lan_ip) {
            router_ip              = Net::parse_ipv4(lan_ip.c_str());
            Net::IPv4Net start_ip  = Net::parse_ipv4(Config::DHCP_POOL_START.c_str());
            Net::IPv4Net end_ip    = Net::parse_ipv4(Config::DHCP_POOL_END.c_str());
            lease_seconds          = Config::DHCP_LEASE_SECONDS;
            init_pool(start_ip, end_ip);
        }

        // Called by Core 1 watchdog when GUI sets dhcp_config_dirty
        void reconfigure(Net::IPv4Net start_ip, Net::IPv4Net end_ip, uint32_t lease_secs) {
            lease_seconds = lease_secs;
            init_pool(start_ip, end_ip);
        }

        // Data plane (Core 3 LAN_RX): intercept and queue request to control plane
        void intercept_request(const Net::ParsedPacket& pkt) {
            DhcpMessage msg;
            msg.len = std::min(pkt.raw_span.size(), size_t(512));
            std::memcpy(msg.data.data(), pkt.raw_span.data(), msg.len);
            request_queue.push(msg);
        }

        // Control plane (Core 1) watchdog: offline parsing and transmission
        void process_background_tasks(int lan_fd) {
            DhcpMessage msg;
            while (request_queue.pop(msg)) {
                handle_dhcp_request(msg, lan_fd);
            }
        }

    private:
        void handle_dhcp_request(DhcpMessage& msg, int lan_fd) {
            auto parsed = Net::ParsedPacket::parse(std::span<uint8_t>(msg.data.data(), msg.len));
            if (!parsed.is_valid_ipv4()) return;
            if (parsed.l4_protocol != 17) return;

            auto udp = parsed.udp();
            if (!udp || ntohs(udp->dest) != 67) return;

            size_t dhcp_offset = parsed.l4_offset + sizeof(Net::UDPHeader);
            if (msg.len < dhcp_offset + sizeof(DhcpHeader)) return;

            auto dhcp = reinterpret_cast<const DhcpHeader*>(msg.data.data() + dhcp_offset);
            
            if (dhcp->op != 1) return; // Only process BootRequest
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
            Net::IPv4Net requested_ip{raw_requested_ip};  // NBO from option 50 wire field

            if (msg_type == 1) { // DHCP Discover
                Net::IPv4Net offered_ip = find_or_assign_lease(dhcp->chaddr);
                if (offered_ip.raw() != 0) {
                    send_dhcp_response(msg.data, parsed, dhcp, 2, offered_ip, lan_fd); // DHCP Offer
                }
            } else if (msg_type == 3) { // DHCP Request
                if (requested_ip.raw() == 0) requested_ip = Net::IPv4Net{dhcp->ciaddr};
                Net::IPv4Net leased_ip = find_or_assign_lease(dhcp->chaddr);

                if (leased_ip == requested_ip) {
                    commit_lease(dhcp->chaddr, leased_ip);
                    send_dhcp_response(msg.data, parsed, dhcp, 5, leased_ip, lan_fd); // DHCP ACK
                    char ip_buf[INET_ADDRSTRLEN]{};
                    uint32_t raw_ip = leased_ip.raw();
                    inet_ntop(AF_INET, &raw_ip, ip_buf, sizeof(ip_buf));
                    std::println("[DHCP Engine] Assigned IP to device: {}", ip_buf);
                } else {
                    send_dhcp_response(msg.data, parsed, dhcp, 6, requested_ip, lan_fd); // DHCP NAK
                }
            }
        }

        Net::IPv4Net find_or_assign_lease(const uint8_t* mac) {
            auto now      = std::chrono::steady_clock::now();
            auto duration = std::chrono::seconds(lease_seconds);

            for (size_t i = 0; i < pool_count; ++i) {
                if (leases[i].active && std::memcmp(leases[i].mac, mac, 6) == 0) return leases[i].ip;
            }
            for (size_t i = 0; i < pool_count; ++i) {
                if (!leases[i].active || now > leases[i].lease_expiry) {
                    std::memcpy(leases[i].mac, mac, 6);
                    leases[i].active       = true;
                    leases[i].lease_expiry = now + duration;
                    return leases[i].ip;
                }
            }
            return Net::IPv4Net{};  // pool exhausted
        }

        void commit_lease(const uint8_t* mac, Net::IPv4Net ip) {
            auto duration = std::chrono::seconds(lease_seconds);
            for (size_t i = 0; i < pool_count; ++i) {
                if (leases[i].ip == ip) {
                    leases[i].active       = true;
                    leases[i].lease_expiry = std::chrono::steady_clock::now() + duration;
                    return;
                }
            }
        }

        void send_dhcp_response(const std::array<uint8_t, 512>& request_data, const Net::ParsedPacket& parsed, const DhcpHeader* req_dhcp, uint8_t type, Net::IPv4Net yiaddr, int lan_fd) {
            alignas(64) std::array<uint8_t, 512> response{};
            
            auto eth = reinterpret_cast<Net::EthernetHeader*>(response.data());
            std::memcpy(eth->dest, req_dhcp->chaddr, 6);
            std::memcpy(eth->src, parsed.eth->dest, 6); // Use our router MAC
            eth->proto = htons(0x0800);

            auto ip = reinterpret_cast<Net::IPv4Header*>(response.data() + sizeof(Net::EthernetHeader));
            ip->ver_ihl = 0x45;
            ip->tos = 0;
            ip->id = 0;
            ip->frag_off = 0;
            ip->ttl = 64;
            ip->protocol = 17;
            ip->saddr = router_ip;
            ip->daddr = Net::IPv4Net{0xFFFFFFFF}; // Broadcast reply
            
            auto udp = reinterpret_cast<Net::UDPHeader*>(response.data() + sizeof(Net::EthernetHeader) + sizeof(Net::IPv4Header));
            udp->source = htons(67);
            udp->dest = htons(68);

            auto dhcp = reinterpret_cast<DhcpHeader*>(response.data() + sizeof(Net::EthernetHeader) + sizeof(Net::IPv4Header) + sizeof(Net::UDPHeader));
            dhcp->op = 2; // BootReply
            dhcp->htype = 1;
            dhcp->hlen = 6;
            dhcp->hops = 0;
            dhcp->xid = req_dhcp->xid;
            dhcp->secs = 0;
            dhcp->flags = htons(0x8000); // Broadcast flag enabled
            dhcp->ciaddr = 0;
            dhcp->yiaddr = yiaddr.raw();     // DhcpHeader wire field — uint32_t NBO
            dhcp->siaddr = router_ip.raw();  // DhcpHeader wire field — uint32_t NBO
            dhcp->giaddr = 0;
            std::memcpy(dhcp->chaddr, req_dhcp->chaddr, 16);
            dhcp->magic_cookie = htonl(0x63825363);

            uint8_t* opt = response.data() + sizeof(Net::EthernetHeader) + sizeof(Net::IPv4Header) + sizeof(Net::UDPHeader) + sizeof(DhcpHeader);
            
            *opt++ = 53; *opt++ = 1; *opt++ = type;
            *opt++ = 1; *opt++ = 4; *opt++ = 255; *opt++ = 255; *opt++ = 255; *opt++ = 0;
            { uint32_t r = router_ip.raw(); *opt++ = 3; *opt++ = 4; std::memcpy(opt, &r, 4); opt += 4; }
            { uint32_t r = router_ip.raw(); *opt++ = 6; *opt++ = 4; std::memcpy(opt, &r, 4); opt += 4; }
            { uint32_t lease_time = htonl(lease_seconds);
              *opt++ = 51; *opt++ = 4; std::memcpy(opt, &lease_time, 4); opt += 4; }
            { uint32_t r = router_ip.raw(); *opt++ = 54; *opt++ = 4; std::memcpy(opt, &r, 4); opt += 4; }
            *opt++ = 255; // DHCP End Indicator

            size_t dhcp_len = (opt - reinterpret_cast<uint8_t*>(dhcp));
            size_t udp_len = sizeof(Net::UDPHeader) + dhcp_len;
            size_t ip_len = sizeof(Net::IPv4Header) + udp_len;
            size_t total_len = sizeof(Net::EthernetHeader) + ip_len;

            ip->tot_len = htons(ip_len);
            
            ip->check = 0;
            uint32_t ip_sum = 0;
            const uint16_t* ip_words = reinterpret_cast<const uint16_t*>(ip);
            for (size_t i = 0; i < sizeof(Net::IPv4Header)/2; ++i) ip_sum += ntohs(ip_words[i]);
            ip_sum = (ip_sum >> 16) + (ip_sum & 0xFFFF);
            ip_sum += (ip_sum >> 16);
            ip->check = htons(~ip_sum);

            udp->len = htons(udp_len);
            udp->check = 0; 
            
            send(lan_fd, response.data(), total_len, MSG_DONTWAIT);
        }
    };
}
