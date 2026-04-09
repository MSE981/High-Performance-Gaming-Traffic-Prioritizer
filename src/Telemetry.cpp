#include "Telemetry.hpp"
#include <sys/eventfd.h>
#include <cstdio>

namespace Scalpel {

void Telemetry::SystemInfo::init_event_fds() {
    rescan_fd_ = ::eventfd(0, EFD_CLOEXEC);
    done_fd_   = ::eventfd(0, EFD_CLOEXEC);
    if (rescan_fd_ < 0 || done_fd_ < 0)
        std::fprintf(stderr, "[Warn] eventfd creation failed — iface refresh button disabled\n");
}

void Telemetry::SystemInfo::request_rescan() {
    if (rescan_fd_ >= 0) ::eventfd_write(rescan_fd_, 1);
}

int Telemetry::SystemInfo::done_notifier_fd() const {
    return done_fd_;
}

void Telemetry::SystemInfo::consume_done() {
    if (done_fd_ >= 0) { uint64_t v; ::eventfd_read(done_fd_, &v); }
}

int Telemetry::SystemInfo::rescan_poll_fd() const {
    return rescan_fd_;
}

void Telemetry::SystemInfo::consume_rescan() {
    if (rescan_fd_ >= 0) { uint64_t v; ::eventfd_read(rescan_fd_, &v); }
}

void Telemetry::SystemInfo::signal_done() {
    if (done_fd_ >= 0) ::eventfd_write(done_fd_, 1);
}

} // namespace Scalpel
