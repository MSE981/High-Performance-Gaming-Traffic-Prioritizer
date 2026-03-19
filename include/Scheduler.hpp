#pragma once
#include <array>
#include <chrono>
#include <vector>
#include <span>
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include <cerrno>
#include <thread>
#include <print>
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


    // 2. 零动态分配环形缓冲区 (大容量对齐版)
    class ZeroAllocRingBuffer {
        // 强制编译器将每个槽位对齐到 4KB (树莓派系统页大小)。
        struct alignas(4096) PacketSlot {
            uint16_t size = 0;
            uint8_t payload[2048]; // 修复：必须声明足够装下标准 1514 字节 MTU 的定长数组，防止内存越界
        };

        // 运行时永不扩容，启动时一次性在堆上划拨连续内存
        std::vector<PacketSlot> pool;
        size_t head = 0;
        size_t tail = 0;
        size_t count = 0;
        const size_t capacity_limit = 8192;

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

    // 封装底层硬件发送结果，完全隔离底层的 errno 脏数据
    enum class TxResult : size_t { Success = 0, Congested = 1, Fatal = 2 };

    inline TxResult try_hardware_send(int fd, std::span<const uint8_t> pkt) {
        for (int retries = 3; retries > 0; --retries, std::this_thread::yield()) {
            if (send(fd, pkt.data(), pkt.size(), MSG_DONTWAIT) >= 0) return TxResult::Success;

            // 只要不是网卡满载 (ENOBUFS/EAGAIN)，其他全部视为致命畸形包
            if (errno != ENOBUFS && errno != EAGAIN) return TxResult::Fatal;
        }
        return TxResult::Congested;
    }

    // 3. 流量整形器
    class Shaper {
        ZeroAllocRingBuffer normal_queue;
        TokenBucket bucket;
        uint64_t trace_counter = 0; // 移至成员变量

        // 定义 Functors 查表数组
        using ResultHandler = void (*)(Shaper*, size_t);
        static constexpr std::array<ResultHandler, 3> result_handlers = {
            // 0: Success (发送成功)[](Shaper* s, size_t) {
                s->normal_queue.pop();
                if (++s->trace_counter % 5000 == 0) {
                    std::println("[Proof-of-Work] Successfully shaped and forwarded 5000 normal packets.");
                }
        },
            // 1: Congested (网卡拥塞)[](Shaper* s, size_t bytes) {
            s->bucket.refund(bytes);
    },
        // 2: Fatal (致命死包)[](Shaper* s, size_t bytes) {
        s->bucket.refund(bytes);
    s->normal_queue.pop(); // 斩断队头死锁
            }
        };

    public:
        explicit Shaper(double limit_mbps) : bucket(limit_mbps) {}

        //允许从外部(如异步测速回调)动态修改限速上限
        void set_rate_limit(double limit_mbps) {
            bucket.set_rate(limit_mbps);
        }

        void enqueue_normal(std::span<const uint8_t> pkt) {
            if (!normal_queue.push(pkt)) {
                Telemetry::instance().dropped_normal.fetch_add(1, std::memory_order_relaxed);

                static time_t last_log = 0;
                time_t now = time(nullptr);
                if (now != last_log) {
                    // 数据驱动优化：利用 C++ 的 bool 隐式转换 (false=0, true=1)
                    static constexpr std::array<const char*, 2> diag_msgs = {
                        "\n[Diag] Drop N: 8192 Queue is FULL! Shaper is throttling/blocking.",
                        "\n[Diag] Drop N: Packet too large (>2048 bytes). GRO is NOT turned off properly!"
                    };
                    std::println(stderr, "{}", diag_msgs[pkt.size() > 2048]);
                    last_log = now;
                }
            }
        }

        void process_queue(int tx_fd) {
            while (!normal_queue.empty()) {
                auto pkt_span = normal_queue.front();

                // 若令牌耗尽，直接跳出等待下一个时间片
                if (!bucket.try_consume(pkt_span.size())) break;

                // 核心解耦：将物理发送请求丢给硬件隔离器，获取严格的枚举状态
                TxResult res = try_hardware_send(tx_fd, pkt_span);

                // Functor 查表分发！
                // 将原先多达 30 行的 if-else 迷宫，压缩为 1 行 O(1) 数组下标直接调用！
                result_handlers[static_cast<size_t>(res)](this, pkt_span.size());

                // 若遇到网卡物理拥塞，终止本轮发包，让网卡喘息
                if (res == TxResult::Congested) break;
            }
        }
    };
}