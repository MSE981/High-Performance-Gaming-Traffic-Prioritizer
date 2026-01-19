#pragma once
#include <span>
#include <cstdint>

// 策略模式接口：支持未来 AI 或 协议解析逻辑的注入
class IPacketProcessor {
public:
    virtual ~IPacketProcessor() = default;
    // 返回 true 则转发，false 则丢弃
    virtual bool process(std::span<const std::uint8_t> packet) = 0;
};

// V1.0 基础实现：透明网桥
class PassThroughProcessor : public IPacketProcessor {
public:
    bool process(std::span<const std::uint8_t> packet) override {
        return !packet.empty(); 
    }
};