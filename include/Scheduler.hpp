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

            // 桶容量设定：允许的最大突发量放宽至 100ms，
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

        // 动态更新令牌桶速率
        void set_rate(double limit_mbps) {
            rate_bytes_per_sec = (limit_mbps * 1e6) / 8.0;
            // 重新根据新速率计算突发容量
            capacity = std::max<double>(15000.0, rate_bytes_per_sec * 0.1);
            // 更新后立即充满令牌，防止切换瞬间产生大量丢包
            tokens = capacity;
            last_refill = std::chrono::steady_clock::now();
        }
    };

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

        //允许从外部(如异步测速回调)动态修改限速上限
        void set_rate_limit(double limit_mbps) {
            bucket.set_rate(limit_mbps);
        }

        void enqueue_normal(std::span<const uint8_t> pkt) {
            if (!normal_queue.push(pkt)) {
                Telemetry::instance().dropped_normal.fetch_add(1, std::memory_order_relaxed);

                // 【诊断探针 1：队列满还是包太大？】(每秒最多打印1次防刷屏)
                static time_t last_log = 0;
                time_t now = time(nullptr);
                if (now != last_log) {
                    if (pkt.size() > 2048) {
                        std::println(stderr, "\n[Diag] Drop N: Packet too large ({} bytes). GRO is NOT turned off properly!", pkt.size());
                    }
                    else {
                        std::println(stderr, "\n[Diag] Drop N: 8192 Queue is FULL! Shaper is throttling/blocking.");
                    }
                    last_log = now;
                }
            }
        }

        void process_queue(int tx_fd) {
            while (!normal_queue.empty()) {
                auto pkt_span = normal_queue.front();

                if (bucket.try_consume(pkt_span.size())) {
                    int retries = 3;
                    bool sent = false;
                    bool fatal_error = false;

                    while (true) {
                        if (send(tx_fd, pkt_span.data(), pkt_span.size(), MSG_DONTWAIT) >= 0) {
                            sent = true;
                            break;
                        }

                        int err = errno; // 捕获瞬间的系统错误码
                        if (err == EMSGSIZE || err == EINVAL) {
                            fatal_error = true;
                            // 【诊断探针 2：发现致命畸形包】
                            static time_t last_log = 0; time_t now = time(nullptr);
                            if (now != last_log) {
                                std::println(stderr, "\n[Diag] Fatal N: EMSGSIZE/EINVAL. Packet size: {}", pkt_span.size());
                                last_log = now;
                            }
                            break;
                        }

                        // 拦截所有未知的错误，统统视为死包防止死锁！
                        if (err != ENOBUFS && err != EAGAIN) {
                            fatal_error = true;
                            // 【诊断探针 3：抓捕未知异常死锁】
                            static time_t last_log = 0; time_t now = time(nullptr);
                            if (now != last_log) {
                                std::println(stderr, "\n[Diag] Unknown Send Error N: errno={} ({})", err, strerror(err));
                                last_log = now;
                            }
                            break;
                        }

                        if (--retries == 0) break;
                        __asm__ __volatile__("yield" ::: "memory");
                    }

                    if (sent) {
                        normal_queue.pop();
                    }
                    else if (fatal_error) {
                        bucket.refund(pkt_span.size());
                        normal_queue.pop();
                    }
                    else {
                        bucket.refund(pkt_span.size());
                        break;
                    }
                }
                else {
                    break;
                }
            }
        }
    };
}