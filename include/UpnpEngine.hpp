#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <print>
#include <cstring>
#include <memory>
#include <unistd.h>
#include "SystemOptimizer.hpp"
#include "NatEngine.hpp"
#include "Config.hpp"

namespace Scalpel::Logic {

    class UpnpEngine {
        std::jthread ssdp_thread;
        std::jthread soap_thread;
        std::atomic<bool> running{true};
        std::shared_ptr<NatEngine> nat_engine;
        std::string router_ip_str;

    public:
        UpnpEngine(std::shared_ptr<NatEngine> nat, const std::string& ip) 
            : nat_engine(nat), router_ip_str(ip) {
            
            ssdp_thread = std::jthread([this]() { run_ssdp_server(); });
            soap_thread = std::jthread([this]() { run_soap_server(); });
            std::println("[UPnP Engine] 启动完成. 监听局域网内主机 SSDP 广播.");
        }

        ~UpnpEngine() {
            running = false;
        }

    private:
        void run_ssdp_server() {
            System::Optimizer::set_current_thread_affinity(1);
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return;

            int opt = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(1900);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            bind(fd, (sockaddr*)&addr, sizeof(addr));

            ip_mreq mreq{};
            mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
            mreq.imr_interface.s_addr = inet_addr(router_ip_str.c_str());
            setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

            // Set recv timeout to handle graceful thread shutdown
            struct timeval tv;
            tv.tv_sec = 1; tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            char buf[1024];
            while (running) {
                sockaddr_in client{};
                socklen_t clen = sizeof(client);
                int n = recvfrom(fd, buf, sizeof(buf)-1, 0, (sockaddr*)&client, &clen);
                if (n > 0) {
                    buf[n] = 0;
                    std::string req(buf);
                    // Minimal check for UPnP IGD Search
                    if (req.find("M-SEARCH") != std::string::npos && 
                        (req.find("urn:schemas-upnp-org:device:InternetGatewayDevice") != std::string::npos ||
                         req.find("ssdp:all") != std::string::npos)) {
                         
                        std::string resp = 
                            "HTTP/1.1 200 OK\r\n"
                            "CACHE-CONTROL: max-age=1800\r\n"
                            "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                            "USN: uuid:12345678-1234-1234-1234-123456789abc::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                            "EXT:\r\n"
                            "Server: Scalpel/1.0 UPnP/1.0 IGD/1.0\r\n"
                            "Location: http://" + router_ip_str + ":5000/desc.xml\r\n"
                            "\r\n";
                            
                        sendto(fd, resp.c_str(), resp.size(), 0, (sockaddr*)&client, clen);
                    }
                }
            }
            close(fd);
        }

        void run_soap_server() {
            System::Optimizer::set_current_thread_affinity(1);
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) return;

            int opt = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(5000);
            addr.sin_addr.s_addr = inet_addr(router_ip_str.c_str());
            bind(fd, (sockaddr*)&addr, sizeof(addr));
            listen(fd, 10);

            struct timeval tv;
            tv.tv_sec = 1; tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            while (running) {
                sockaddr_in client{};
                socklen_t clen = sizeof(client);
                int cfd = accept(fd, (sockaddr*)&client, &clen);
                if (cfd < 0) continue;

                char buf[2048];
                int n = recv(cfd, buf, sizeof(buf)-1, 0);
                if (n > 0) {
                    buf[n] = 0;
                    std::string req(buf);
                    
                    if (req.find("GET /desc.xml") != std::string::npos) {
                        std::string xml = "<?xml version=\"1.0\"?>\r\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n  <URLBase>http://" + router_ip_str + ":5000/</URLBase>\r\n  <device>\r\n    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>\r\n    <friendlyName>Scalpel Gaming Engine</friendlyName>\r\n    <serviceList>\r\n      <service>\r\n        <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>\r\n        <serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>\r\n        <controlURL>/control</controlURL>\r\n        <eventSubURL>/event</eventSubURL>\r\n        <SCPDURL>/scpd.xml</SCPDURL>\r\n      </service>\r\n    </serviceList>\r\n  </device>\r\n</root>";
                        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: " + std::to_string(xml.size()) + "\r\nConnection: close\r\n\r\n" + xml;
                        send(cfd, resp.c_str(), resp.size(), 0);
                    } else if (req.find("POST /control") != std::string::npos) {
                        auto body_idx = req.find("\r\n\r\n");
                        if (body_idx != std::string::npos) {
                            std::string body = req.substr(body_idx + 4);
                            if (body.find("AddPortMapping") != std::string::npos) {
                                auto get_tag = [&](const std::string& t) -> std::string {
                                    auto start = body.find("<" + t + ">");
                                    auto end = body.find("</" + t + ">");
                                    if (start != std::string::npos && end != std::string::npos) {
                                        return body.substr(start + t.size() + 2, end - (start + t.size() + 2));
                                    }
                                    return "";
                                };
                                
                                std::string ext_port = get_tag("NewExternalPort");
                                std::string int_port = get_tag("NewInternalPort");
                                std::string int_client = get_tag("NewInternalClient");
                                std::string protocol = get_tag("NewProtocol");

                                if (!ext_port.empty() && !int_client.empty()) {
                                    uint16_t eP = std::stoi(ext_port);
                                    uint16_t iP = int_port.empty() ? eP : std::stoi(int_port);
                                    uint8_t proto = (protocol.find("TCP") != std::string::npos) ? 6 : 17;
                                    uint32_t cIP = inet_addr(int_client.c_str());

                                    nat_engine->add_upnp_rule(eP, cIP, iP, proto);
                                    std::println("[UPnP] 游戏主机开放端口申请: {} [{}] -> {}:{} 已放行至 Data Plane.", ext_port, protocol, int_client, iP);
                                    
                                    std::string resp_xml = "<?xml version=\"1.0\"?><s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:AddPortMappingResponse xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"/></s:Body></s:Envelope>";
                                    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: " + std::to_string(resp_xml.size()) + "\r\nConnection: close\r\n\r\n" + resp_xml;
                                    send(cfd, resp.c_str(), resp.size(), 0);
                                }
                            }
                        }
                    }
                }
                close(cfd);
            }
            close(fd);
        }
    };
}
