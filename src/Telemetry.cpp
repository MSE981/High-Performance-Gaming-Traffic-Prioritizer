#include "Telemetry.hpp"
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>

namespace HPGTP {

std::expected<void, std::string> Telemetry::SystemInfo::init_event_fds() {
    rescan_fd_ = ::eventfd(0, EFD_CLOEXEC);
    if (rescan_fd_ < 0)
        return std::unexpected(std::string("eventfd (rescan): ") + std::strerror(errno));
    done_fd_ = ::eventfd(0, EFD_CLOEXEC);
    if (done_fd_ < 0) {
        const int e = errno;
        ::close(rescan_fd_);
        rescan_fd_ = -1;
        return std::unexpected(std::string("eventfd (done): ") + std::strerror(e));
    }
    return {};
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

} // namespace HPGTP
