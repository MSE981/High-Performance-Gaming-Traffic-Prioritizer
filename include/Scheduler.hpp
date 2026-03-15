#pragma once
#include <chrono>
#include <vector>
#include <span>
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include <cerrno> // 必须引入：用于智能区分“硬件忙碌”与“致命废包”
#include "Headers.hpp"
#include "Telemetry.hpp"

namespace Scalpel::Traffic {

    // 1. 令牌桶算法：精确控制普通流量的最高带宽
    class TokenBucket {
        double tokens; // 使用 double 防止高频微秒级循环下小数部分的带宽被截断丢失
        double capacity;
        double rate_bytes_per_sec;
        std::chrono::time_point<std::chrono::steady_clock> last_refill;

    public:
        explicit TokenBucket(double limit_mbps) {
            // 将 Mbps 转换为 每秒字节数
            rate_bytes_per_sec = (limit_mbps * 1e6) / 8.0;

            // 桶容量设定：允许的最大突发量放宽至 100ms，完美适配现代网页与下载的突发特性
            capacity = std::max<double>(15000.0, rate_bytes_per_sec * 0.1);
            tokens = capacity;
            last_refill = std::chrono::steady_clock::now();
        }

        void refill() {
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> dt = now - last_refill;
            double new_tokens = dt.count() * rate_bytes_per_sec;

            if (new_tokens > 0) {
                tokens = std::min(capacity, tokens + new_tokens);
                last_refill = now;
            }
        }

        bool try_consume(uint32_t bytes) {
            refill();
            if (tokens >= bytes) {
                tokens -= bytes;
                return true;
            }
            return false;
        }

        // 用于在网卡拥塞或遇到死包发送失败时，退还被提前扣除的令牌
        void refund(uint32_t bytes) {
            tokens = std::min(capacity, tokens + static_cast<double>(bytes));
        }
    };

    // 2. 零动态分配环形缓冲区 (大容量对齐版)
    class ZeroAllocRingBuffer {
        // 核心黑科技：alignas(4096) 
        // 强制编译器将每个槽位对齐到 4KB (树莓派系统页大小)。
        // 彻底消除 TLB Miss，大幅提升 CPU 寻址效率。
        struct alignas(4096) PacketSlot {
            uint16_t size = 0;
            uint8_t payload[2048]; // 修复：必须声明足够装下标准 1514 字节 MTU 的定长数组，防止内存越界
        };

        // 运行时永不扩容，启动时一次性在堆上划拨连续内存
        std::vector<PacketSlot> pool;
        size_t head = 0;
        size_t tail = 0;
        size_t count = 0;
        const size_t capacity_limit = 8192; // 修复：扩大 8 倍容量，彻底接住 Steam 等工具的突发流量

    public:
        ZeroAllocRingBuffer() : pool(8192) {}

        bool push(std::span<const uint8_t> pkt) {
            // AQM 机制：队列满时实施 Tail Drop，主动丢弃下载包触发 TCP 降速
            if (count == capacity_limit || pkt.size() > 2048) {
                return false;
            }

            auto& slot = pool[tail]; // 修复：补齐缺失的尾部索引
            slot.size = static_cast<uint16_t>(pkt.size());
            // 极速内存拷贝
            std::memcpy(slot.payload, pkt.data(), pkt.size());

            tail = (tail + 1) % capacity_limit;
            count++;
            return true;
        }

        std::span<const uint8_t> front() const {
            if (count == 0) return {};
            const auto& slot = pool[head]; // 修复：补齐缺失的头部索引
            return { slot.payload, slot.size };
        }

        void pop() {
            if (count > 0) {
                head = (head + 1) % capacity_limit;
                count--;
            }
        }

        bool empty() const { return count == 0; }
    };

    // 3. 流量整形器
    class Shaper {
        ZeroAllocRingBuffer normal_queue;
        TokenBucket bucket;

    public:
        explicit Shaper(double limit_mbps) : bucket(limit_mbps) {}

        // 将普通包送入缓存排队
        void enqueue_normal(std::span<const uint8_t> pkt) {
            if (!normal_queue.push(pkt)) {
                // 如果 push 失败（队列已满 8192 或超大包），记录一次真实的主动拥塞丢包 (AQM)
                Telemetry::instance().dropped_normal.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // 抽空队列：由 worker 线程在每一轮循环中调用
        void process_queue(int tx_fd) {
            while (!normal_queue.empty()) {
                auto pkt_span = normal_queue.front();

                // 检查令牌是否足够发这个包
                if (bucket.try_consume(pkt_span.size())) {
                    int retries = 3;
                    bool sent = false;
                    bool fatal_error = false;

                    while (true) {
                        if (send(tx_fd, pkt_span.data(), pkt_span.size(), MSG_DONTWAIT) >= 0) {
                            sent = true;
                            break;
                        }

                        // 智能区分：这是永久发不出去的畸形包，还是网卡真的满载了？
                        if (errno == EMSGSIZE || errno == EINVAL) {
                            fatal_error = true;
                            break;
                        }

                        if (--retries == 0) break;
                        __asm__ __volatile__("yield" ::: "memory");
                    }

                    if (sent) {
                        normal_queue.pop(); // 发送成功，出队销毁
                    }
                    else if (fatal_error) {
                        // 致命畸形死包：永远发不出去。必须冷酷销毁，否则会导致队列永久死锁！
                        bucket.refund(pkt_span.size());
                        normal_queue.pop(); // 斩断队头死锁
                    }
                    else {
                        // 网卡是真的满载了 (ENOBUFS/EAGAIN)
                        // 绝对不能 pop()！把合法包留在队首，依靠 8192 容量的队列来缓冲！
                        // 真正的拥塞丢包(AQM)只让它发生在 enqueue_normal 队列塞满时。
                        bucket.refund(pkt_span.size());
                        break; // 退出本轮发包，让网卡喘息
                    }
                }
                else {
                    break; // 令牌耗尽，退出循环让包继续排队
                }
            }
        }
    };
}