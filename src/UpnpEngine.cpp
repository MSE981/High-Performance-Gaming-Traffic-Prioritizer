#include "UpnpEngine.hpp"
#include "DataPlane.hpp"
#include "SystemOptimizer.hpp"
#include <span>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <print>
#include <string_view>
#include <charconv>

namespace Scalpel::Logic {

UpnpEngine::UpnpEngine(std::shared_ptr<NatEngine> nat, const std::string& ip)
    : nat_engine(nat), router_ip_str(ip) {
    soap_job_notify_efd = ::eventfd(0, EFD_CLOEXEC);
    if (soap_job_notify_efd < 0) {
        std::println(stderr,
            "[UPnP] soap job eventfd failed ({}); UPnP disabled.",
            std::strerror(errno));
        return;
    }
    running.store(true, std::memory_order_relaxed);
    ssdp_thread        = std::thread([this]() { run_ssdp_server(); });
    soap_worker_thread = std::thread([this]() { run_soap_worker(); });
    soap_thread        = std::thread([this]() { run_soap_server(); });
    std::println("[UPnP Engine] Startup complete. Listening for LAN SSDP broadcasts.");
}

UpnpEngine::~UpnpEngine() {
    running.store(false, std::memory_order_relaxed);
    if (soap_job_notify_efd >= 0)
        (void)::eventfd_write(soap_job_notify_efd, 1);
    if (soap_listen_fd >= 0)
        (void)::shutdown(soap_listen_fd, SHUT_RDWR);
    if (soap_thread.joinable()) soap_thread.join();
    if (soap_worker_thread.joinable()) soap_worker_thread.join();
    if (soap_job_notify_efd >= 0) {
        ::close(soap_job_notify_efd);
        soap_job_notify_efd = -1;
    }
    if (ssdp_thread.joinable()) ssdp_thread.join();
}

void UpnpEngine::run_ssdp_server() {
    System::Optimizer::set_current_thread_affinity(1);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(1900);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    mreq.imr_interface.s_addr = inet_addr(router_ip_str.c_str());
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    struct timeval tv{ .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[1024];
    while (running.load(std::memory_order_relaxed)) {
        sockaddr_in client{};
        socklen_t clen = sizeof(client);
        int n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                         reinterpret_cast<sockaddr*>(&client), &clen);
        if (n > 0) {
            std::string_view req(buf, n);
            if (req.find("M-SEARCH") != std::string_view::npos &&
                (req.find("urn:schemas-upnp-org:device:InternetGatewayDevice") != std::string_view::npos ||
                 req.find("ssdp:all") != std::string_view::npos)) {
                char resp[512];
                int resp_len = snprintf(resp, sizeof(resp),
                    "HTTP/1.1 200 OK\r\n"
                    "CACHE-CONTROL: max-age=1800\r\n"
                    "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                    "USN: uuid:12345678-1234-1234-1234-123456789abc::"
                        "urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                    "EXT:\r\n"
                    "Server: HPGTP/1.0 UPnP/1.0 IGD/1.0\r\n"
                    "Location: http://%s:5000/desc.xml\r\n"
                    "\r\n", router_ip_str.c_str());
                sendto(fd, resp, resp_len, 0,
                       reinterpret_cast<sockaddr*>(&client), clen);
            }
        }
    }
    close(fd);
}

void UpnpEngine::run_soap_worker() {
    System::Optimizer::set_current_thread_affinity(1);
    for (;;) {
        SoapRequestJob job;
        while (soap_jobs.pop(job))
            dispatch_soap_http(job.cfd, std::string_view(job.buf.data(), job.len));

        if (!running.load(std::memory_order_relaxed))
            break;

        struct pollfd pfd{};
        pfd.fd     = soap_job_notify_efd;
        pfd.events = POLLIN;
        int pr     = ::poll(&pfd, 1, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((pfd.revents & POLLIN) != 0) {
            uint64_t v;
            (void)::eventfd_read(soap_job_notify_efd, &v);
        }
    }
}

void UpnpEngine::dispatch_soap_http(int cfd, std::string_view req) {
    if (req.find("GET /desc.xml") != std::string_view::npos) {
        const char* xml_template =
            "<?xml version=\"1.0\"?>\r\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
            "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
            "  <URLBase>http://%s:5000/</URLBase>\r\n"
            "  <device>\r\n    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>\r\n"
            "    <friendlyName>High-Performance Gaming Traffic Prioritizer</friendlyName>\r\n"
            "    <serviceList>\r\n      <service>\r\n"
            "        <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>\r\n"
            "        <serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>\r\n"
            "        <controlURL>/control</controlURL>\r\n"
            "        <eventSubURL>/event</eventSubURL>\r\n"
            "        <SCPDURL>/scpd.xml</SCPDURL>\r\n"
            "      </service>\r\n    </serviceList>\r\n  </device>\r\n</root>";
        char xml_buf[1200];
        int xml_len = snprintf(xml_buf, sizeof(xml_buf), xml_template, router_ip_str.c_str());
        if (xml_len >= static_cast<int>(sizeof(xml_buf)))
            xml_len = static_cast<int>(sizeof(xml_buf)) - 1;

        char resp[1500];
        int resp_len = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
            xml_len, xml_buf);
        DataPlane::TxFrameOutput::send_stream_blocking(
            cfd,
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(resp),
                                      static_cast<size_t>(resp_len)));
    } else if (req.find("POST /control") != std::string_view::npos) {
        auto body_idx = req.find("\r\n\r\n");
        if (body_idx != std::string_view::npos && body_idx + 4 < req.size()) {
            std::string_view body = req.substr(body_idx + 4);
            if (body.find("AddPortMapping") != std::string_view::npos) {
                auto get_tag = [&](std::string_view t) -> std::string_view {
                    if (t.size() > 60) return {};
                    char start_tag[64], end_tag[64];
                    int sl = snprintf(start_tag, sizeof(start_tag), "<%.*s>",
                                     static_cast<int>(t.size()), t.data());
                    int el = snprintf(end_tag, sizeof(end_tag), "</%.*s>",
                                     static_cast<int>(t.size()), t.data());
                    auto s = body.find(std::string_view(start_tag, sl));
                    auto e = body.find(std::string_view(end_tag, el));
                    if (s != std::string_view::npos && e != std::string_view::npos)
                        return body.substr(s + sl, e - (s + sl));
                    return {};
                };

                auto ext_port   = get_tag("NewExternalPort");
                auto int_port   = get_tag("NewInternalPort");
                auto int_client = get_tag("NewInternalClient");
                auto protocol   = get_tag("NewProtocol");

                if (!ext_port.empty() && !int_client.empty()) {
                    uint16_t eP = 0;
                    std::from_chars(ext_port.data(), ext_port.data() + ext_port.size(), eP);
                    uint16_t iP = eP;
                    if (!int_port.empty())
                        std::from_chars(int_port.data(), int_port.data() + int_port.size(), iP);
                    uint8_t proto = (protocol.find("TCP") != std::string_view::npos) ? 6 : 17;

                    char ip_buf[32]{};
                    std::memcpy(ip_buf, int_client.data(),
                                std::min(int_client.size(), size_t(31)));
                    Net::IPv4Net cIP = Net::parse_ipv4(ip_buf);
                    nat_engine->add_upnp_rule({eP, cIP, iP, proto});
                    std::println("[UPnP] Port mapping accepted: {} [{}] -> {}:{} forwarded to data plane.",
                                 ext_port, protocol, int_client, iP);

                    const char* resp_xml =
                        "<?xml version=\"1.0\"?>"
                        "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                        "<s:Body><u:AddPortMappingResponse "
                        "xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"/>"
                        "</s:Body></s:Envelope>";
                    char resp_buf[512];
                    int resp_len = snprintf(resp_buf, sizeof(resp_buf),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\n"
                        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                        std::strlen(resp_xml), resp_xml);
                    DataPlane::TxFrameOutput::send_stream_blocking(
                        cfd,
                        std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>(resp_buf),
                            static_cast<size_t>(resp_len)));
                }
            }
        }
    }
    ::close(cfd);
}

void UpnpEngine::run_soap_server() {
    System::Optimizer::set_current_thread_affinity(1);
    soap_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (soap_listen_fd < 0) return;

    int opt = 1;
    setsockopt(soap_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(5000);
    addr.sin_addr.s_addr = inet_addr(router_ip_str.c_str());
    bind(soap_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(soap_listen_fd, 10);

    struct timeval tv{ .tv_sec = 1, .tv_usec = 0 };
    setsockopt(soap_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (running.load(std::memory_order_relaxed)) {
        sockaddr_in client{};
        socklen_t clen = sizeof(client);
        int cfd = accept(soap_listen_fd, reinterpret_cast<sockaddr*>(&client), &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        struct timeval ctv{ .tv_sec = 1, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));

        char buf[2048];
        int n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            ::close(cfd);
            continue;
        }

        SoapRequestJob job{};
        job.cfd = cfd;
        job.len = static_cast<uint16_t>(n);
        std::memcpy(job.buf.data(), buf, static_cast<size_t>(n));
        if (!soap_jobs.push(job)) {
            static const char busy[] =
                "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
            (void)::send(cfd, busy, sizeof(busy) - 1, MSG_NOSIGNAL);
            ::close(cfd);
        } else {
            (void)::eventfd_write(soap_job_notify_efd, 1);
        }
    }
    if (soap_listen_fd >= 0) {
        ::close(soap_listen_fd);
        soap_listen_fd = -1;
    }
}

} // namespace Scalpel::Logic
