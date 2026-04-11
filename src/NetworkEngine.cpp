#include "NetworkEngine.hpp"
#include <print>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <linux/if_packet.h>

namespace Scalpel::Engine {

RawSocketManager::RawSocketManager(std::string_view iface_name) {
    iface_name.copy(iface.data(), IFACE_NAME_MAX - 1);
}

RawSocketManager::~RawSocketManager() {
    if (ring && ring != MAP_FAILED) munmap(ring, ring_size);
    if (fd >= 0) close(fd);
}

std::expected<void, std::string> RawSocketManager::init() {
    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) return std::unexpected(std::string("Socket creation failed: ") + strerror(errno));

    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface.data(), IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) return std::unexpected("Interface lookup failed");

    struct ifreq ifr_p{};
    std::strncpy(ifr_p.ifr_name, iface.data(), IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr_p) < 0)
        return std::unexpected("Failed to get interface flags");

    ifr_p.ifr_flags |= IFF_PROMISC;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr_p) < 0)
        return std::unexpected("Failed to set IFF_PROMISC. Check permissions.");
    std::println("[Engine] Interface {} set to promiscuous mode", iface.data());

    tpacket_req req{
        .tp_block_size = BLOCK_SIZE,
        .tp_block_nr   = BLOCK_NR,
        .tp_frame_size = FRAME_SIZE,
        .tp_frame_nr   = FRAME_NR
    };
    if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0)
        return std::unexpected("Setsockopt RX_RING failed");

    ring_size = static_cast<size_t>(req.tp_block_size) * req.tp_block_nr;
    ring = static_cast<uint8_t*>(mmap(nullptr, ring_size,
                                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (ring == MAP_FAILED) return std::unexpected("mmap failed");

    sockaddr_ll sll{};
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex  = ifr.ifr_ifindex;
    if (bind(fd, reinterpret_cast<sockaddr*>(&sll), sizeof(sll)) < 0)
        return std::unexpected("Bind failed");

    return {};
}

void RawSocketManager::do_poll(int timeout_ms) {
    struct pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = POLLIN;
    poll(&pfd, 1, timeout_ms);
}

bool RawSocketManager::peek_frame(std::span<uint8_t>& out) {
    while (true) {
        auto* hdr = reinterpret_cast<tpacket_hdr*>(ring + (rx_idx * FRAME_SIZE));
        if (!(hdr->tp_status & TP_STATUS_USER)) return false;

        auto* sll = reinterpret_cast<sockaddr_ll*>(
            reinterpret_cast<uint8_t*>(hdr) + TPACKET_ALIGN(sizeof(tpacket_hdr)));

        if (sll->sll_pkttype != PACKET_OUTGOING) {
            out = std::span<uint8_t>{
                reinterpret_cast<uint8_t*>(hdr) + hdr->tp_mac, hdr->tp_len };
            return true;
        }

        // PACKET_OUTGOING: release silently and check next frame
        hdr->tp_status = TP_STATUS_KERNEL;
        rx_idx = (rx_idx + 1) % FRAME_NR;
    }
}

void RawSocketManager::advance_frame() {
    auto* hdr = reinterpret_cast<tpacket_hdr*>(ring + (rx_idx * FRAME_SIZE));
    hdr->tp_status = TP_STATUS_KERNEL;
    rx_idx = (rx_idx + 1) % FRAME_NR;
}

} // namespace Scalpel::Engine
