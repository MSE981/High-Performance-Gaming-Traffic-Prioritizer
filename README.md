# GamingTrafficPrioritizer

A lightweight IPv4 gateway-style software router for a Raspberry Pi, written in C++23. It prioritizes gaming/traffic-sensitive packets over bulk downloads using two-tier QoS, and includes user-space forwarding, NAT and a DHCP server.

## Purpose

Large downloads and video streaming cause latency spikes for online games on a normal router. This project sits between the upstream router and the LAN, inspects each packet in user space and forwards it with a fixed pipeline: DHCP intercept, NAT rewrite, Ethernet rewrite and QoS routing. Gaming and small control packets are sent on an unthrottled High lane; bulk large-packet traffic is rate-limited on the Normal lane.

## System operation

The Pi exposes two Ethernet interfaces:

```text
Internet -- Router -- [eth0 WAN | HPGTP | eth1 LAN] -- Switch / devices (Core 2 = WAN->LAN, Core 3 = LAN->WAN)
```

Execution is split across the four cores:

- Cores 2 & 3: the two forwarding directions (RX + processing threads).
- Cores 0 & 1: GUI, watchdog/control loop, DHCP background tasks, self-test and shutdown-save helpers.

## Core features

- Raw `AF_PACKET` RX ring + bounded frame queue; allocation-free hot path.
- Fixed-size hash tables and lock-free per-core telemetry (no locks on the packet path).
- Two-tier QoS: packets below `LARGE_PACKET_THRESHOLD` and DNS go to High; larger packets to Normal (rate-limited).
- User-space NAT (SNAT / DNAT / ICMP mapping with session expiry).
- DHCP server with a GUI-editable pool and lease duration (applied via an eventfd callback).
- Qt6 dashboard with live High/Normal Mb charts, QoS and service controls.
- CPU frequency locked to the performance governor.

## Hardware and software

- Raspberry Pi (Cortex-A72, 4 cores), with a second Ethernet adapter for the LAN side.
- GCC 14, CMake, Qt 6 Widgets and `ethtool`.

## Build and run

```bash
chmod +x ./start_release.sh
sudo ./start_release.sh
```

The script installs missing packages, builds the release binary (demo targets are not built) and launches the GUI on the DSI display. The executable is `build/GamingTrafficPrioritizer`; keep the repo root as the working directory so `config/config.txt` resolves.

## Configuration

All keys in `config/config.txt` are optional. The GUI rewrites the file on a clean exit.

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

`ROUTER_IP` is the gateway handed to DHCP clients and must share the LAN subnet with the DHCP pool. The WAN IP used for NAT is always taken from the kernel (NetworkManager) address on `IFACE_WAN`.

## Termination

Use the GUI **Shutdown** button. It stops the data plane and control loop gracefully, and saves configuration.

## License

MIT. See [`LICENSE`](LICENSE).
