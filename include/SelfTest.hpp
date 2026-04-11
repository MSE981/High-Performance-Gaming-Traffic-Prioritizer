#pragma once
// §2.2.1  std::function functor callback
// §2.2.3  Complex result delivered as struct, not multi-arg
// §3.3.1  Worker runs inside std::thread
// §3.3.2  Caller joins the thread after completion
// §2.3.7  Hardware checks via raw fd (/sys, /proc) — implementations in SelfTest.cpp
//
// POSIX headers (<netinet/in.h>, <sys/socket.h>, <fcntl.h>, <unistd.h>, <dirent.h>)
// and engine headers are confined to SelfTest.cpp; this header exposes only the
// public interface types Report, TestCase, and SelfTest.
#include <array>
#include <cstring>
#include <functional>
#include <thread>
#include <cstdint>

#include "Headers.hpp"

namespace Scalpel::SelfTest {

// ─────────────────────────────────────────────────────────────────────────────
// §2.2.3: Complex callback data encapsulated in struct (not raw multi-arg)
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
// SelfTest — publisher/subscriber pattern (§2.2.1)
//   start()          → launches std::thread (§3.3.1)
//   join()           → caller blocks until worker done (§3.3.2)
//   registerCallback → accepts std::function functor (§2.2.1)
// ─────────────────────────────────────────────────────────────────────────────
class SelfTest {
public:
    // §2.2.1: Register a std::function functor as result callback
    using ResultCallback = std::function<void(const Report&)>;
    void registerCallback(ResultCallback cb) { callback_ = std::move(cb); }

    ~SelfTest() { if (uthread_.joinable()) uthread_.join(); }

    // §3.3.1: Spawn worker thread
    void start() { uthread_ = std::thread(&SelfTest::run, this); }

    // §3.3.2: Join — caller blocks here until run() completes
    void join() { if (uthread_.joinable()) uthread_.join(); }

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
};

// Last self-test report — written once before GUI starts, read by Dashboard on init
inline Report LAST_REPORT{};

} // namespace Scalpel::SelfTest
