#pragma once
#include <array>
#include <string>
#include <string_view>
#include <thread>
#include <atomic>
#include <memory>
#include "Headers.hpp"
#include "NatEngine.hpp"

namespace HPGTP::Logic {

    class UpnpEngine {
        struct SoapRequestJob {
            int cfd = -1;
            uint16_t len = 0;
            std::array<char, 2048> buf{};
        };

        std::thread ssdp_thread;
        std::thread soap_thread;
        std::thread soap_worker_thread;
        std::atomic<bool> running{false};
        std::shared_ptr<NatEngine> nat_engine;
        std::string router_ip_str;

        Net::SpscRingBuffer<SoapRequestJob, 64> soap_jobs{};
        int soap_listen_fd = -1;
        int soap_job_notify_efd = -1;
        int shutdown_efd = -1;

        void run_ssdp_server();
        void run_soap_server();
        void run_soap_worker();
        void dispatch_soap_http(int cfd, std::string_view req);

    public:
        // Bit 0 = SSDP bind failed, Bit 1 = SOAP bind failed, Bit 2 = SOAP listen failed
        std::atomic<uint8_t> bind_errors{0};

        UpnpEngine(std::shared_ptr<NatEngine> nat, const std::string& ip);
        ~UpnpEngine();
    };
}
