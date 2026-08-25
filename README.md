# GamingTrafficPrioritizer

A small software router for a Raspberry Pi. It runs in userspace, reads packets with AF_PACKET, and prioritises gaming traffic over bulk downloads. Written in C++23.

The Pi sits between the upstream router and the local network. One Ethernet port faces the internet (WAN), the other faces your devices (LAN).

## What it does

The router puts every IPv4 packet into one of two lanes.

- High lane: gaming and control traffic. Small packets, DNS, ICMP and TCP handshakes use this lane. It is not rate limited.
- Normal lane: bulk traffic. Large packets use this lane. A token bucket caps the total rate.

The global download and upload caps are set in the GUI, in Mb (megabits per second).

## What is in it

- Userspace forwarding over a raw AF_PACKET RX ring and a bounded frame queue.
- Two-tier QoS with a configurable packet-size threshold.
- User-space NAT with SNAT, DNAT, ICMP mapping and session expiry.
- A DHCP server with a GUI-editable address pool and lease time.
- A Qt6 dashboard on the DSI display. It updates at 60 Hz and shows the High/Normal Mb rates, the QoS controls, and a read-only device list.
- CPU frequency locked to the performance governor.

## Hardware and software

- Raspberry Pi with a Cortex-A72 CPU (4 cores) and a second Ethernet adapter.
- C++23, GCC 14, CMake, Qt 6 Widgets, and ethtool.

## Build and run

```bash
chmod +x ./start_release.sh
sudo ./start_release.sh
```

The script checks the dependencies, compiles the release binary and starts the GUI on the DSI display. The binary is `build/GamingTrafficPrioritizer`. Run it from the repo root so it finds `config/config.txt`.

The data plane needs raw sockets and the ability to change interface flags, so run as root. NetworkManager owns the WAN address.

## Configuration

`config/config.txt` holds the defaults. Every key is optional. The GUI rewrites this file when you close the app.

```ini
IFACE_WAN=eth0
IFACE_LAN=eth1
ROUTER_IP=192.168.12.1
APPLY_ROUTER_IP_TO_LAN=true
LAN_PREFIX_LEN=24

ENABLE_ACCELERATION=true
LARGE_PACKET_THRESHOLD=1000

enable_nat=true
enable_dhcp=true
DHCP_POOL_START=192.168.12.50
DHCP_POOL_END=192.168.12.255
```

A few notes:

- `ROUTER_IP` is the gateway handed to DHCP clients. It must be in the same subnet as the DHCP pool.
- `APPLY_ROUTER_IP_TO_LAN` sets the kernel address on the LAN interface at startup. Turn it off if you use netplan or systemd-networkd.
- `LARGE_PACKET_THRESHOLD` is the packet size in bytes. Smaller packets go to the High lane.
- `ENABLE_ACCELERATION=true` turns on QoS. Set it to false for a plain bridge.
- NAT uses the WAN address that NetworkManager assigns. There is no static WAN IP setting.

## Architecture

The four CPU cores take separate jobs.

- Cores 2 and 3 run the two forwarding directions (WAN to LAN, LAN to WAN). Each direction has a receive thread and a processing thread pinned to that core.
- Cores 0 and 1 run the GUI, the watchdog service threads, the DHCP controller and the shutdown helper.

The packet path is event driven. Threads wait with blocking `poll()` on sockets and eventfds. The Shaper reports each send result through a `std::function` callback, and packet events go to a small callback registry. The engines, shapers and forwarding state live in `App`. Workers borrow them through raw pointers. `ForwardingPlane` owns the L2/ARP snapshot and the WAN IP lookup.

## What this is not

- Not a firewall or a DNS cache. Those services were removed.
- Not a multi-WAN router and it does not bridge several LAN ports. One WAN, one LAN.
- Not headless. The GUI is the only start path.
- It is a userspace router. The kernel forwards only where the start script sets the rules.

## License

MIT. See [`LICENSE`](LICENSE).
