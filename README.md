# GamingTrafficPrioritizer

**A dedicated Raspberry Pi 5 network device that prioritizes gaming data over general network traffic using microsecond precision.**

GamingTrafficPrioritizer is software built in C++23 using Linux real-time programming concepts. It inspects network packets using raw sockets and fixed-size, allocation-free data structures to operate as an **IPv4 gateway-style forwarder** between WAN and LAN interfaces , not as a classic layer-2 transparent bridge.

---

## Purpose

Network latency often increases when high-bandwidth activities, such as large downloads or video streaming, occur simultaneously with online multiplayer gaming. Standard router Quality of Service (QoS) configurations require manual maintenance and can exhibit inconsistent performance.

GamingTrafficPrioritizer is designed to resolve this latency variation immediately at the network layer. It operates as an intermediary device on a Raspberry Pi, analyzing network packets and executing routing decisions strictly based on traffic properties. The system ensures that gaming packets receive immediate transmission priority over high-bandwidth traffic.

---

## System Operation

```text
                        ┌───────────────────────────────────────────┐
Internet ──── Router ──▶│ eth0 [GamingTrafficPrioritizer Pi 5] eth1 ──▶ Switch / devices
                        │                                           │
                        │  Core 0 ▸ Qt dashboard                    │
                        │  Core 1 ▸ Watchdog · NAT · DHCP           │
                        │  Core 2 ▸ WAN → LAN  (download)           │
                        │  Core 3 ▸ LAN → WAN  (upload)             │
                        └───────────────────────────────────────────┘
```

The system classifies every network packet into one of three distinct categories before transmission:

| Category | Traffic Type | Forwarding Protocol |
|----------|---------------|-----------|
| **Critical** | DNS with UDP/TCP port 53; very small TCP segments | Immediate forwarding |
| **High** | Gaming packets, small UDP data | Zero-wait forwarding |
| **Normal** | Large downloads, video streaming | Rate-controlled forwarding |

The classifier is automated. It analyzes packet sizes and generation frequencies to identify continuous high-volume traffic. This automation removes the requirement for users to configure and manage manual port lists for individual games.

---

## Core Features

- **High-throughput RX path**: Receives frames via a Linux `AF_PACKET` RX ring . Each frame is copied into an internal bounded queue for parsing and forwarding; the hot path avoids extra allocation but is **not** end-to-end zero-copy from ring to wire.
- **Lock-free processing**: Implements fixed-size hash tables to eliminate memory allocation and threading locks during active network transmission.
- **Dedicated CPU core allocation**: Assigns specific runtime tasks to individual processing cores to prevent context-switching delays.
- **Real-time execution**: Employs synchronous system calls for periodic tasks, avoiding blocking timeout functions in the primary packet forwarding cycle.
- **Device bandwidth limits**: Applies a configurable rate limiter for individual IP addresses, with capabilities to update these limits instantly during execution.
- **Integrated network services**: Includes NAT , a DHCP server, a DNS cache, UPnP/IGD mapping, and a stateful firewall. All services can be controlled directly via the interface.
- **Local dashboard**: Features a custom Qt6 interface displaying real-time packet rates, core performance metrics, and service controls using Direct Rendering Manager hardware acceleration.
- **Interactive system notifications**: Displays system alerts via a graphical user interface panel without disrupting background network processing.
- **Command-line mode**: With `enable_gui=false`, no dashboard window is shown. The binary is still dynamically linked to Qt 6; install compatible Qt 6 runtime libraries on the target system even when running headless.

---

## System Requirements

### Hardware

| Component | Specification |
|------|-------|
| Raspberry Pi 5 | 4 GB RAM version recommended |
| Secondary Ethernet Adapter | USB 3.0 Gigabit Ethernet adapter for Local Area Network (LAN) |
| Display Screen | Optional: Waveshare 8.0" 1280×800 or compatible screen for local interaction |

**Connection Diagram:**

```text
Internet ──── Router ──── [Pi eth0 | Pi eth1] ──── Switch / User Devices
```

For single-device configurations, connect the destination device directly to the `eth1` port.

### Software Dependencies

| Package | Purpose |
|---------|-----|
| GCC 14 and CMake 3.20+ | Required for compiling C++23 standard library components |
| `qt6-base-dev` | Required to build the local dashboard interface |
| `qt6-qpa-plugins` | Required for direct Direct Rendering Manager display output |
| `ethtool` | Needed to disable hardware offloads that alter raw-socket packet properties |
| `speedtest-cli` | Optional module for automated bandwidth capability measurement |

---

## Installation and Execution

```bash
# 1. Download the repository source
git clone <repo-url>
cd High-Performance-Gaming-Traffic-Prioritizer

# 2. Build and launch: start.sh runs start_release.sh, which on Debian-based
#    systems can install missing build/runtime packages (e.g. via apt), then builds if needed.
sudo ./start_release.sh

# Headless (no Qt window): set enable_gui=false in config/config.txt, then run the
# binary with the current working directory set to the repository root so
# config/config.txt is found (paths are relative to CWD):
```

First-time setup on a minimal image usually requires installing compiler, CMake, Qt development packages, and related tools—either manually or by relying on `start_release.sh` where applicable. After a successful build, the main executable is `build/GamingTrafficPrioritizer`; keep using the repo root as the working directory when launching so configuration and assets resolve correctly. The process runs until you stop it ; it is not a one-shot command.

---

## Configuration Variables

The program loads `config/config.txt` relative to the **current working directory** . All parameters are optional; the system will initialize with default variables if the file is missing. The local graphical interface writes active parameters to this file on clean exit when saving is enabled.

```ini
# ── Network interface assignment ──────────────────────────────────────
IFACE_WAN=eth0           # Router-facing interface (Internet connection)
IFACE_LAN=eth1           # Device-facing interface (Local network)
ROUTER_IP=192.168.12.1   # Pi LAN gateway IP (DHCP server, local services)
WAN_IP=                  # Optional NAT address; empty = use IFACE_WAN IPv4 from the kernel

# ── Interface mode definition ─────────────────────────────────────────
# Roles are configured by the Graphical Interface
# Allowed values: gateway | wan | lan | disabled
IFACE_GATEWAY=eth0

# ── Visualization ─────────────────────────────────────────────────────
enable_gui=true          # Assign 'false' for direct console operations

# ── Traffic classification ────────────────────────────────────────────
ENABLE_ACCELERATION=true
LARGE_PACKET_THRESHOLD=1000   # Packet byte count defining substantial traffic
PUNISH_TRIGGER_COUNT=30       # Consecutive large packets required to trigger rate-limiting
CLEANUP_INTERVAL=10000        # Periodic interval for flow table removal

# ── Network service initialization ────────────────────────────────────
enable_nat=true
enable_dhcp=true
enable_dns_cache=true
enable_upnp=true
enable_firewall=true

# ── DHCP pool (optional; defaults match ROUTER_IP subnet in built-in defaults) ──
# DHCP_POOL_START=192.168.12.50
# DHCP_POOL_END=192.168.12.255

# ── Specialized device configurations ──────────────────────────────────
# IP_LIMIT=192.168.12.50:20    # Enforce a 20 Mbps bandwidth maximum to specific IP
# IP_LIMIT=192.168.12.51:5     # Enforce a 5 Mbps bandwidth maximum to specific IP
```

---


### Safe Termination

| Action Method | System Response |
|--------|-------------|
| Dashboard Interface → **Shutdown** | Application terminates; configurations preserved |
| Graphical Interface → **Close (×)** | Application terminates; configurations preserved |
| Command Terminal → `Ctrl+C` | Application terminates; configurations preserved |

---

## Technical Specifications

A summary of the primary software capabilities:

**Network Forwarding Execution (Cores 2 & 3)**

Each independent processing core runs a continuous network evaluation cycle mapped directly to kernel memory interactions. Incoming network packets pass through a statically compiled execution schedule. This structure guarantees linear evaluation and prevents conditional processing delays:

```text
[DHCP Assessment] → [DNS Assessment] → [NAT Routing] → [Rate-Limiting] → [QoS Routine]
```

The specified routing procedure assigns memory paths strictly based on array indexing routines.

**Layer-2 egress on forwarded IPv4**

Frames are transmitted with `AF_PACKET` `send()` using the full Ethernet header from the buffer. After NAT rewrites the IPv4 header, the pipeline sets **Ethernet source** to the egress interface MAC and **destination** to the next hop (default gateway on WAN, or the LAN host from the kernel ARP cache on LAN). DHCP replies use the **LAN interface MAC** as the Ethernet source (not the destination of the inbound broadcast). Ensure the WAN interface has a default route and that `ROUTER_IP` and the DHCP pool share the LAN subnet.

**Network Data Structuring**

The primary classification mechanism relies on a fixed memory hash table implementation. It guarantees instantaneous assessment capacity without relying on progressive memory reallocation limits.

**Constant Measurement Evaluation (Core 1)**

Runs on a one-second `timerfd` tick. The same thread reads hardware temperature and system metrics using normal file I/O on `/sys` and `/proc` (`open`, `read`, `close`) and aggregates statistics. On each tick it also runs periodic maintenance for NAT, DNS, DHCP, and firewall engines (e.g. `tick()` / background tasks), applies pending GUI or config-driven updates via dirty flags and atomics, and refreshes telemetry—so it routinely uses system calls and is not “syscall-free” for those subsystems.

**High Availability Updates**

Bandwidth application adjustments are dynamically written into inactive parameter arrays. Upon receiving a system command, an atomic control assignment seamlessly redirects ongoing data flows into the updated variable configuration blocks without interrupting concurrent data movement.

---

## Project Roadmap

- [√] Primary QoS transmission framework
- [√] Application of adjustable device network caps
- [√] Embedded integration for standard NAT, DHCP, DNS, and Firewall processing
- [√] Real-time graphical dashboard processing
- [√] Headless operational capability
- [√] Direct DRM graphics visualization integration
- [√] Integrated notification response interfaces
- [O] Remote network administration API configuration

## Social Media

https://www.reddit.com/r/homelab/comments/1sq50o7/highperformancegamingtrafficprioritizer/?solution=64704c72d53bc35364704c72d53bc353&js_challenge=1&token=bbbe4bf1c9a2b5160829c4be34da5861a8bd7ba03ea8fb9885c4add909ff7214&utm_source=share&utm_medium=web3x&utm_name=web3xcss&utm_term=1&utm_content=share_button

---

## License

This software is released under the MIT License. See the [`LICENSE`](LICENSE) file.
