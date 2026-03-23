#pragma once
#include <span>
#include <chrono>
#include <array>
#include <cstdint>
#include <netinet/in.h>
#include "Headers.hpp"
#include "Processor.hpp"

namespace Scalpel::Logic {
    // 渐进式校验和更新 (RFC 1624): HC' = ~(~HC + ~m + m')
    // 在用户态极速完成 IP/Port 转换时的必备算法，免除全量重新校验的性能消耗
    static inline void update_checksum_16(uint16_t& check, uint16_t old_val, uint16_t new_val) {
        uint32_t sum = (~ntohs(check) & 0xFFFF) + (~ntohs(old_val) & 0xFFFF) + ntohs(new_val);
        sum = (sum & 0xFFFF) + (sum >> 16);
        check = htons(~(sum + (sum >> 16)));
    }
    
    static inline void update_checksum_32(uint16_t& check, uint32_t old_val, uint32_t new_val) {
        update_checksum_16(check, old_val & 0xFFFF, new_val & 0xFFFF);
        update_checksum_16(check, old_val >> 16, new_val >> 16);
    }

    // 真正的零拷贝应用态 NAT 引擎
    class NatEngine {
        struct NatSession {
            FlowKey internal_key; // saddr=LAN_IP, sport=LAN_Port, daddr=WAN_DEST, dport=WAN_DEST_PORT
            uint16_t external_port;
            uint32_t last_active_tick;
            bool active = false;
        };

        // 准则 3.1: 零动态分配。所有表项在构造时定死在内存中，杜绝使用 std::unordered_map
        static constexpr size_t MAX_SESSIONS = 65536;
        
        // 正向散列表：LAN -> WAN (出网时查表掩盖私网 IP)
        std::array<NatSession, MAX_SESSIONS> table{};
        // 反向映射表：External Port -> Session Index (入网时 O(1) 极速查找，直接数组寻址！)
        std::array<int32_t, 65536> port_to_index{};
        
        uint16_t next_port = 10000;
        uint32_t wan_ip;
        uint32_t current_tick = 0;

        uint32_t hash_flow(const FlowKey& k) const {
            uint32_t h = 2166136261U;
            auto proc = [&](const auto& val) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
                for(size_t i=0; i<sizeof(val); ++i) { h ^= p[i]; h *= 16777619U; }
            };
            proc(k.saddr); proc(k.daddr); proc(k.sport); proc(k.dport);
            return h;
        }

    public:
        explicit NatEngine() : wan_ip(0) {
            port_to_index.fill(-1);
        }

        void set_wan_ip(uint32_t ip) { wan_ip = ip; }
        
        // 低频调用的 Tick 驱动器 (接驳 Watchdog 解耦高频系统调用)
        void tick() { current_tick++; }

        // 出网流量 (SNAT): 替换源 IP 与源端口
        bool process_outbound(std::span<uint8_t> pkt) {
            if (wan_ip == 0 || pkt.size() < sizeof(Net::EthernetHeader) + sizeof(Net::IPv4Header)) return false;
            
            auto ip = reinterpret_cast<Net::IPv4Header*>(pkt.data() + sizeof(Net::EthernetHeader));
            size_t ihl = (ip->ver_ihl & 0x0F) * 4;
            if (ip->protocol != 6 && ip->protocol != 17) return false;

            uint16_t* sport_ptr = nullptr;
            uint16_t* check_ptr = nullptr;
            uint16_t dport = 0;

            size_t l4_offset = sizeof(Net::EthernetHeader) + ihl;
            if (ip->protocol == 17) { 
                auto udp = reinterpret_cast<Net::UDPHeader*>(pkt.data() + l4_offset);
                sport_ptr = &udp->source; dport = udp->dest; check_ptr = &udp->check;
            } else { 
                auto tcp = reinterpret_cast<Net::TCPHeader*>(pkt.data() + l4_offset);
                sport_ptr = &tcp->source; dport = tcp->dest; check_ptr = &tcp->check;
            }

            FlowKey key{ip->saddr, ip->daddr, *sport_ptr, dport};
            uint32_t h = hash_flow(key) % MAX_SESSIONS;
            
            uint16_t ext_port = 0;

            for (size_t i = 0; i < 32; ++i) { // 限制最大线性探测避免死锁
                size_t idx = (h + i) % MAX_SESSIONS;
                if (!table[idx].active || (current_tick - table[idx].last_active_tick > 300)) {
                    // 腾出旧端口
                    if (table[idx].active) port_to_index[ntohs(table[idx].external_port)] = -1;
                    
                    table[idx].internal_key = key;
                    table[idx].external_port = htons(next_port++);
                    if (next_port > 60000) next_port = 10000;
                    table[idx].active = true;
                    table[idx].last_active_tick = current_tick;
                    ext_port = table[idx].external_port;
                    port_to_index[ntohs(ext_port)] = idx;
                    break;
                }
                if (table[idx].internal_key == key) {
                    table[idx].last_active_tick = current_tick;
                    ext_port = table[idx].external_port;
                    break;
                }
            }

            if (!ext_port) return false; // NAT TABLE FULL

            // 直接在内存 span 视图上执行零拷贝 O(1) 覆写
            update_checksum_32(ip->check, ip->saddr, wan_ip);
            if (check_ptr && *check_ptr != 0) {
                update_checksum_32(*check_ptr, ip->saddr, wan_ip);
                update_checksum_16(*check_ptr, *sport_ptr, ext_port);
            }

            ip->saddr = wan_ip;
            *sport_ptr = ext_port;

            return true;
        }

        // 入网流量 (DNAT): 替换目的 IP 与目的端口
        bool process_inbound(std::span<uint8_t> pkt) {
            if (wan_ip == 0 || pkt.size() < sizeof(Net::EthernetHeader) + sizeof(Net::IPv4Header)) return false;

            auto ip = reinterpret_cast<Net::IPv4Header*>(pkt.data() + sizeof(Net::EthernetHeader));
            size_t ihl = (ip->ver_ihl & 0x0F) * 4;

            if (ip->protocol != 6 && ip->protocol != 17) return false;
            if (ip->daddr != wan_ip) return false; // 只有发给公网口的包才做转换

            uint16_t* dport_ptr = nullptr;
            uint16_t* check_ptr = nullptr;
            uint16_t sport = 0;

            size_t l4_offset = sizeof(Net::EthernetHeader) + ihl;
            if (ip->protocol == 17) {
                auto udp = reinterpret_cast<Net::UDPHeader*>(pkt.data() + l4_offset);
                dport_ptr = &udp->dest; sport = udp->source; check_ptr = &udp->check;
            } else {
                auto tcp = reinterpret_cast<Net::TCPHeader*>(pkt.data() + l4_offset);
                dport_ptr = &tcp->dest; sport = tcp->source; check_ptr = &tcp->check;
            }

            // O(1) 绝对定址查找反向映射
            int32_t idx = port_to_index[ntohs(*dport_ptr)];
            if (idx == -1 || !table[idx].active || 
                table[idx].internal_key.daddr != ip->saddr || 
                table[idx].internal_key.dport != sport) {
                return false; 
            }

            table[idx].last_active_tick = current_tick;
            uint32_t internal_ip = table[idx].internal_key.saddr;
            uint16_t internal_port = table[idx].internal_key.sport;

            // O(1) 修改并校准
            update_checksum_32(ip->check, ip->daddr, internal_ip);
            if (check_ptr && *check_ptr != 0) {
                update_checksum_32(*check_ptr, ip->daddr, internal_ip);
                update_checksum_16(*check_ptr, *dport_ptr, internal_port);
            }

            ip->daddr = internal_ip;
            *dport_ptr = internal_port;

            return true;
        }
    };
}
