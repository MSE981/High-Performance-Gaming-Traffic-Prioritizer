#pragma once
#include <span>
#include <chrono>
#include <array>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <netinet/in.h>
#include <sys/socket.h>
#include <print>
#include "Headers.hpp"

namespace Scalpel::Logic {
    struct DnsHeader {
        uint16_t id;
        uint16_t flags;
        uint16_t qdcount;
        uint16_t ancount;
        uint16_t nscount;
        uint16_t arcount;
    };

    struct DnsMessage {
        size_t len;
        std::array<uint8_t, 512> data;
    };

    // 核心分离式 DNS 解析引警
    class DnsEngine {
        // 静态表：遵守准则 3.1: Zero Dynamic Allocation
        struct DnsCacheEntry {
            uint32_t domain_hash = 0;
            uint32_t ipv4_address = 0;
            uint32_t expire_tick = 0;
            std::atomic<bool> valid{false}; 
        };

        static constexpr size_t CACHE_SIZE = 4096;
        std::array<DnsCacheEntry, CACHE_SIZE> cache{};
        Net::SpscRingBuffer<DnsMessage, 1024> response_queue{}; 
        
        uint32_t current_tick = 0;

        // FNV-1a Hash for domain string (QNAME)
        static uint32_t hash_qname(const uint8_t* qname, size_t max_len) {
            uint32_t h = 2166136261U;
            size_t i = 0;
            while (i < max_len && qname[i] != 0) { // DNS 标签以 0 终止
                h ^= qname[i]; h *= 16777619U;
                i++;
            }
            return h;
        }

    public:
        void tick() { current_tick++; }

        // Core 3 (Upstream: LAN -> WAN) - 数据面命中与零拷贝覆写反弹
        bool process_query(Net::ParsedPacket& pkt, int bounce_fd) {
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

            // O(1) 原地哈希
            uint32_t h = hash_qname(pkt.raw_span.data() + qname_offset, pkt.raw_span.size() - qname_offset);
            size_t idx = h % CACHE_SIZE;

            // 准则 3.0: 纯解引用的 Acquire 屏障实现无锁读取
            if (!cache[idx].valid.load(std::memory_order_acquire)) return false;
            if (cache[idx].domain_hash != h || current_tick > cache[idx].expire_tick) {
                cache[idx].valid.store(false, std::memory_order_relaxed);
                return false;
            }

            // --- Cache Hit: Zero-Copy Bounce (原地修改，零内存复制) ---
            uint32_t cached_ip = cache[idx].ipv4_address;
            size_t old_len = pkt.raw_span.size();
            size_t new_len = old_len + 16;
            if (new_len > 1500) return false; // 防止 MTU 溢出

            dns->flags = htons(ntohs(dns->flags) | 0x8180); // 置为 Response (QR=1)
            dns->ancount = htons(1);

            // 强插 16 bytes A Record 到 packet 尾部（假定 RawSocket mmap ring 预留了足够 padding）
            uint8_t* tail = pkt.raw_span.data() + old_len;
            tail[0] = 0xC0; tail[1] = 0x0C; // Name Pointer 回指 Question
            tail[2] = 0x00; tail[3] = 0x01; // Type A
            tail[4] = 0x00; tail[5] = 0x01; // Class IN
            tail[6] = 0x00; tail[7] = 0x00; tail[8] = 0x01; tail[9] = 0x2C; // TTL = 300s
            tail[10] = 0x00; tail[11] = 0x04; // IP 长度 = 4
            std::memcpy(&tail[12], &cached_ip, 4);

            for (int i=0; i<6; ++i) { std::swap(pkt.eth->src[i], pkt.eth->dest[i]); }
            
            uint32_t s_ip = pkt.ipv4->saddr;
            pkt.ipv4->saddr = pkt.ipv4->daddr;
            pkt.ipv4->daddr = s_ip;
            pkt.ipv4->tot_len = htons(new_len - sizeof(Net::EthernetHeader));
            
            pkt.ipv4->check = 0;
            uint32_t ip_sum = 0;
            const uint16_t* ip_words = reinterpret_cast<const uint16_t*>(pkt.ipv4);
            for(size_t i=0; i<pkt.ihl/2; ++i) ip_sum += ntohs(ip_words[i]);
            ip_sum = (ip_sum >> 16) + (ip_sum & 0xFFFF);
            ip_sum += (ip_sum >> 16);
            pkt.ipv4->check = htons(~ip_sum);

            uint16_t s_port = udp->source;
            udp->source = udp->dest;
            udp->dest = s_port;
            udp->len = htons(new_len - sizeof(Net::EthernetHeader) - pkt.ihl);
            udp->check = 0; 

            // Fast Bounce! 原地以物理极速将其打回内网口 (`bounce_fd` = `rx_fd`)
            send(bounce_fd, pkt.raw_span.data(), new_len, MSG_DONTWAIT);
            return true; // 阻断数据面流水线
        }

        // Core 2 (Downstream: WAN -> LAN) - 数据面外网响应截留，推向 Control Plane 异步处理
        void intercept_response(const Net::ParsedPacket& pkt) {
            if (pkt.raw_span.size() > 512 || !pkt.is_valid_ipv4()) return;
            if (pkt.l4_protocol != 17) return;

            auto udp = pkt.udp();
            if (!udp || ntohs(udp->source) != 53) return; // 只监听外部发来的 DNS 数据

            DnsMessage msg;
            msg.len = pkt.raw_span.size();
            std::memcpy(msg.data.data(), pkt.raw_span.data(), pkt.raw_span.size());
            // 无休止向无锁队列中推送（由 C1 看门狗稍后吸收），若溢出丢弃不造成阻塞
            response_queue.push(msg); 
        }

        // Control Plane (Core 1) 后台慢速学习循环（剥离自高频数据流）
        void process_background_tasks() {
            DnsMessage msg;
            int counter = 0;
            while (response_queue.pop(msg) && counter++ < 32) { // 每秒最多拆分 32 个包，绝不能吃尽 Watchdog
                auto ip = reinterpret_cast<const Net::IPv4Header*>(msg.data.data() + sizeof(Net::EthernetHeader));
                size_t ihl = (ip->ver_ihl & 0x0F) * 4;
                size_t dns_offset = sizeof(Net::EthernetHeader) + ihl + sizeof(Net::UDPHeader);
                
                auto dns = reinterpret_cast<const DnsHeader*>(msg.data.data() + dns_offset);
                if (ntohs(dns->ancount) == 0) continue;

                size_t qname_offset = dns_offset + sizeof(DnsHeader);
                if (qname_offset >= msg.len) continue;
                
                uint32_t h = hash_qname(msg.data.data() + qname_offset, msg.len - qname_offset);

                // 解包过程极其复杂且充满不确定性指针：绝佳证明了剥离到 Core 1 是正确的决定
                size_t ptr = qname_offset;
                while (ptr < msg.len && msg.data[ptr] != 0) { ptr += msg.data[ptr] + 1; }
                ptr += 5; // skip null byte (1) + qtype (2) + qclass (2)

                // 第一条 Answer
                if (ptr + 12 <= msg.len) {
                    // Type A (0x0001) Record
                    if (msg.data[ptr+2] == 0x00 && msg.data[ptr+3] == 0x01) {
                        uint32_t ipv4 = *reinterpret_cast<const uint32_t*>(&msg.data[ptr+10]);
                        
                        size_t idx = h % CACHE_SIZE;
                        cache[idx].domain_hash = h;
                        cache[idx].ipv4_address = ipv4;
                        cache[idx].expire_tick = current_tick + 300; // 有效期 (TICK 单位)
                        // 准则 3.0：Release 屏障确保上面的内存写完毕后，数据面才能 acquire 到 True
                        cache[idx].valid.store(true, std::memory_order_release);
                        
                        // std::println("[Control Plane] Learned DNS Record Hash:{}", h);
                    }
                }
            }
        }
    };
}


