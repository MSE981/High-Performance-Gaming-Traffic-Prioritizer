# GamingTrafficPrioritizer

**A dedicated Raspberry Pi 5 network device that prioritizes gaming data over general network traffic using microsecond precision.**

GamingTrafficPrioritizer is software built in C++23 using Linux real-time programming concepts. It inspects network packets using raw sockets and lock-free data structures to function as a transparent network bridge.

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
| **Critical** | DNS queries, TCP handshakes | Immediate forwarding |
| **High** | Gaming packets, small UDP data | Zero-wait forwarding |
| **Normal** | Large downloads, video streaming | Rate-controlled forwarding |

The classifier is automated. It analyzes packet sizes and generation frequencies to identify continuous high-volume traffic. This automation removes the requirement for users to configure and manage manual port lists for individual games.

---

## Core Features

- **Zero-copy network forwarding**: Utilizes the Linux kernel ring buffer (`TPACKET` mmap) to process packet data without copying it between kernel and userspace memory areas, maximizing processing speed.
- **Lock-free processing**: Implements fixed-size hash tables to eliminate memory allocation and threading locks during active network transmission.
- **Dedicated CPU core allocation**: Assigns specific runtime tasks to individual processing cores to prevent context-switching delays.
- **Real-time execution**: Employs synchronous system calls for periodic tasks, avoiding blocking timeout functions in the primary packet forwarding cycle.
- **Device bandwidth limits**: Applies a configurable rate limiter for individual IP addresses, with capabilities to update these limits instantly during execution.
- **Integrated network services**: Includes NAT (SNAT/DNAT), a DHCP server, a DNS cache, UPnP/IGD mapping, and a stateful firewall. All services can be controlled directly via the interface.
- **Local dashboard**: Features a custom Qt6 interface displaying real-time packet rates, core performance metrics, and service controls using Direct Rendering Manager (DRM) hardware acceleration.
- **Interactive system notifications**: Displays system alerts via a graphical user interface panel without disrupting background network processing.
- **Command-line mode**: Supports efficient execution without physical displays via direct configuration settings.

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
| `qt6-qpa-plugins` | Required for direct Direct Rendering Manager (DRM) display output |
| `ethtool` | Needed to disable hardware offloads that alter raw-socket packet properties |
| `speedtest-cli` | Optional module for automated bandwidth capability measurement |

---

## Installation and Execution

```bash
# 1. Download the repository source
git clone <repo-url>
cd High-Performance-Gaming-Traffic-Prioritizer

# 2. Build and launch the system (this script automatically manages dependencies)
sudo ./start.sh

# Complete headless boot sequence (no graphical interface)
# Set enable_gui=false in the configuration file, then execute:
cd build && sudo ./GamingTrafficPrioritizer
```

The system requires no external installation steps beyond compilation; the binary executable is located inside the `build/` directory and functions persistently.

---

## Configuration Variables

The system relies on a `config.txt` file located in the application directory. All parameters are optional; the system will initialize with default variables. The local graphical interface writes active parameters to this file automatically during the termination process.

```ini
# ── Network interface assignment ──────────────────────────────────────
IFACE_WAN=eth0           # Router-facing interface (Internet connection)
IFACE_LAN=eth1           # Device-facing interface (Local network)
ROUTER_IP=192.168.1.100  # Device LAN IP address for DHCP and NAT services

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
enable_pppoe=false

# ── Specialized device configurations ──────────────────────────────────
# IP_LIMIT=192.168.1.50:20     # Enforce a 20 Mbps bandwidth maximum to specific IP
# IP_LIMIT=192.168.1.51:5      # Enforce a 5 Mbps bandwidth maximum to specific IP

# ── Data link layer protocols ─────────────────────────────────────────
ENABLE_STP=false
ENABLE_IGMP_SNOOPING=false
```

---

## Managing the Service

```bash
# Launch with the graphical dashboard visualization:
sudo ./start.sh

# Launch as a standard network process:
cd build && sudo ./GamingTrafficPrioritizer
```

The execution protocol verifies dependency availability, compiles new iterations, enforces required adapter states, and directs initialization.

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

**Network Data Structuring**

The primary classification mechanism relies on a fixed memory hash table implementation. It guarantees instantaneous assessment capacity without relying on progressive memory reallocation limits.

**Constant Measurement Evaluation (Core 1)**

Configured exactly to a 1-second interval tracking schedule. This specific core performs hardware temperature validation and system metrics tracking utilizing explicit file descriptor readings (`open()`, `read()`, `close()`), ensuring strict state synchronicity. In parallel, it actively manages NAT status evaluation, DNS memory assessment, and DHCP lease confirmation as pure, lock-free in-memory operations (via atomic updates and message queues) entirely avoiding system call overhead.

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


---

## License

This software is released under the MIT License.
