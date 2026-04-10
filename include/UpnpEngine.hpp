#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include "NatEngine.hpp"

namespace Scalpel::Logic {

    class UpnpEngine {
        std::thread ssdp_thread;
        std::thread soap_thread;
        std::atomic<bool> running{false};
        std::shared_ptr<NatEngine> nat_engine;
        std::string router_ip_str;

        void run_ssdp_server();
        void run_soap_server();

    public:
        UpnpEngine(std::shared_ptr<NatEngine> nat, const std::string& ip);
        ~UpnpEngine();
    };
}
