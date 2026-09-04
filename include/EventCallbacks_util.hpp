#pragma once
#include <array>
#include <functional>
#include <cstddef>
#include <span>
#include "Headers_util.hpp"
#include "Telemetry.hpp"
#include "Scheduler_util.hpp"

namespace HPGTP::Utils::Events {

// Interface subscriber for data-plane events
class PacketObserver {
public:
    virtual ~PacketObserver() = default;
    virtual void on_frame(std::span<const uint8_t> frame) = 0;
    virtual void on_packet(const Utils::Net::ParsedPacket& pkt, Utils::Net::Priority priority) = 0;
    virtual void on_batch(const Telemetry::BatchStats& stats, int core_id) = 0;
    virtual void on_tx_result(Engine::Scheduler::TxResult result, size_t bytes) = 0;
};

// std::function subscriber slots 
using FrameCallback  = std::function<void(std::span<const uint8_t>)>;
using PacketCallback = std::function<void(const Utils::Net::ParsedPacket&, Utils::Net::Priority)>;
using BatchCallback  = std::function<void(const Telemetry::BatchStats&, int)>;

// Fixed-size callback registry.
class CallbackRegistry {
public:
    static constexpr size_t MAX_OBSERVERS = 8;

    void register_observer(PacketObserver* observer) {
        if (observer && observer_count_ < MAX_OBSERVERS)
            observers_[observer_count_++] = observer;
    }

    void set_frame_callback(FrameCallback cb)  { frame_cb_  = std::move(cb); }
    void set_packet_callback(PacketCallback cb) { packet_cb_ = std::move(cb); }
    void set_batch_callback(BatchCallback cb)   { batch_cb_  = std::move(cb); }

    void dispatch_frame(std::span<const uint8_t> frame) const {
        for (size_t i = 0; i < observer_count_; ++i)
            observers_[i]->on_frame(frame);
        if (frame_cb_) frame_cb_(frame);
    }

    void dispatch_packet(const Utils::Net::ParsedPacket& pkt, Utils::Net::Priority priority) const {
        for (size_t i = 0; i < observer_count_; ++i)
            observers_[i]->on_packet(pkt, priority);
        if (packet_cb_) packet_cb_(pkt, priority);
    }

    void dispatch_batch(const Telemetry::BatchStats& stats, int core_id) const {
        for (size_t i = 0; i < observer_count_; ++i)
            observers_[i]->on_batch(stats, core_id);
        if (batch_cb_) batch_cb_(stats, core_id);
    }

    void dispatch_tx_result(Engine::Scheduler::TxResult result, size_t bytes) const {
        for (size_t i = 0; i < observer_count_; ++i)
            observers_[i]->on_tx_result(result, bytes);
    }

private:
    std::array<PacketObserver*, MAX_OBSERVERS> observers_{};
    size_t observer_count_ = 0;
    FrameCallback  frame_cb_;
    PacketCallback packet_cb_;
    BatchCallback  batch_cb_;
};

} // namespace HPGTP::Utils::Events
