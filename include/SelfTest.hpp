#pragma once
// Callback delivery uses std::function; structured types carry multi-field results.
// The worker runs on a std::thread; the caller should join() after start().
// Hardware checks use raw fds on /sys and /proc (implementations in SelfTest.cpp).
//
// POSIX headers (<netinet/in.h>, <sys/socket.h>, <fcntl.h>, <unistd.h>, <dirent.h>)
// and engine headers are confined to SelfTest.cpp; this header exposes only the
// public interface types Report, TestCase, and SelfTest.
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <thread>
#include <cstdint>

#include "Headers.hpp"

namespace HPGTP::SelfTest {

// ─────────────────────────────────────────────────────────────────────────────
// Test case row: name, pass flag, and short human-readable detail.
// ─────────────────────────────────────────────────────────────────────────────
struct TestCase {
    std::array<char, 64>  name{};
    bool                  pass{false};
    std::array<char, 128> detail{};
};

struct Report {
    std::array<TestCase, 32> cases{};
    size_t count  = 0;
    size_t passed = 0;

    void add(const char* n, bool p, const char* d) {
        if (count >= 32) return;
        auto& c = cases[count++];
        std::strncpy(c.name.data(),   n, 63);  c.name[63]   = '\0';
        std::strncpy(c.detail.data(), d, 127); c.detail[127] = '\0';
        c.pass = p;
        if (p) ++passed;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// SelfTest — worker thread plus optional result callback.
//   start()          -> launches std::thread running run()
//   join()           -> blocks until the worker finishes
//   registerCallback -> stores std::function<void(const Report&)>
// ─────────────────────────────────────────────────────────────────────────────
class SelfTest {
public:
    using ResultCallback = std::function<void(const Report&)>;
    void registerCallback(ResultCallback cb) { callback_ = std::move(cb); }

    ~SelfTest() { if (uthread_.joinable()) uthread_.join(); }

    // Ignores duplicate start while a worker is still joinable (call join() first).
    void start() {
        if (uthread_.joinable()) return;
        uthread_ = std::thread([this] {
            struct Active {
                std::atomic<bool>& a;
                explicit Active(std::atomic<bool>& x) : a(x) {
                    a.store(true, std::memory_order_relaxed);
                }
                ~Active() { a.store(false, std::memory_order_relaxed); }
            } active{worker_running_};
            (void)active;
            run();
        });
    }

    void join() { if (uthread_.joinable()) uthread_.join(); }

    bool worker_running() const {
        return worker_running_.load(std::memory_order_relaxed);
    }

private:
    void run();

    // ── Packet builders — implementations in SelfTest.cpp ───────────────────
    static std::array<uint8_t, 42>  make_udp_pkt(Net::IPv4Net sip, Net::IPv4Net dip,
                                                   uint16_t sport, uint16_t dport);
    static std::array<uint8_t, 54>  make_tcp_pkt(Net::IPv4Net sip, Net::IPv4Net dip,
                                                   uint16_t sport, uint16_t dport,
                                                   uint16_t flags);
    static std::array<uint8_t, 512> make_dns_query(Net::IPv4Net sip, Net::IPv4Net dip,
                                                    const char* host_dotted,
                                                    size_t& out_len);
    static std::array<uint8_t, 512> make_dhcp_discover(size_t& out_len);

    // ── Sub-tests — implementations in SelfTest.cpp ─────────────────────────
    void test_nat(Report& r);
    void test_dns(Report& r);
    void test_dhcp(Report& r);
    void test_firewall(Report& r);
    void test_classifier(Report& r);
    void test_system(Report& r);

    ResultCallback callback_;
    std::thread    uthread_;
    std::atomic<bool> worker_running_{false};
};

// Last self-test report — written once before GUI starts, read by Dashboard on init
inline Report LAST_REPORT{};

} // namespace HPGTP::SelfTest
