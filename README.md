# High-Performance Gaming Traffic Prioritizer

Turn a Raspberry Pi 5 into a dedicated network appliance that sits between your router and your devices. It automatically identifies gaming and latency-sensitive traffic, forwards it without delay, and smoothly throttles heavy background downloads — so your ping stays stable even while someone else is streaming 4K video or pulling a large game update.

---

## How It Works

The prioritizer runs as a transparent network bridge. Every packet that enters one port is inspected and classified before being forwarded out the other port. Three priority levels are enforced in real time:

| Priority | Traffic type | Behaviour |
|----------|-------------|-----------|
| Critical | DNS queries, TCP handshakes | Bypasses all queues — sent immediately |
| High | Gaming packets, HTTPS | Forwarded with zero queuing delay |
| Normal | Large downloads, streaming | Passed through a rate-controlled buffer |

A heuristic flow tracker identifies heavy flows automatically by monitoring packet sizes and frequencies. No manual port configuration is required for most games.

---

## Features

- **Real-time QoS** — three-tier traffic classification with no manual rule setup for common games
- **Per-device bandwidth caps** — optionally limit download speed for individual IP addresses
- **Built-in services** — NAT, DHCP server, DNS cache, UPnP/IGD, stateful firewall, PPPoE client; each can be toggled independently
- **Qt6 dashboard** — local GUI showing live packet rate and bandwidth charts, per-core statistics, interface role assignment, QoS rules, and service toggles
- **Headless / CLI mode** — runs over SSH with no display required
- **Auto-save** — all settings are written back to `config.txt` on every clean exit; GUI changes are persisted automatically

---

## System Requirements

### Hardware

| Item | Notes |
|------|-------|
| Raspberry Pi 5 | Any RAM variant |
| Second Ethernet adapter | USB-to-Ethernet is fine; this becomes your LAN port |
| Display (optional) | Only required for GUI mode |

Connect the Pi between your router and your devices:

```
Internet → Router → [eth0  Pi  eth1] → Switch / devices
```

### Software

| Package | Purpose |
|---------|---------|
| GCC 14 + CMake 3.20+ | Build toolchain |
| `qt6-base-dev` | GUI dashboard |
| `libgpiod-dev` (v2) | LED status indicators |
| `ethtool` | Disables hardware offloads that interfere with packet inspection |
| `speedtest-cli` | ISP bandwidth auto-measurement |

---

## Installation

### 1. Install dependencies

```bash
sudo apt update
sudo apt install build-essential cmake gcc-14 g++-14 \
    libgpiod-dev libgpiod2 \
    qt6-base-dev qt6-base-dev-tools \
    ethtool speedtest-cli
```

### 2. Build

```bash
git clone <repo-url>
cd High-Performance-Gaming-Traffic-Prioritizer
mkdir build && cd build
cmake ..
cmake --build . -j4
```

The binary `GamingTrafficPrioritizer` is placed in `build/`.

---

## Configuration

Create `config.txt` in the same directory as the binary. All fields are optional — the program starts with sensible defaults if the file is absent. When using the GUI, settings are saved back to this file automatically on exit.

```ini
# ── Network interfaces ────────────────────────────────────────
# If you use the GUI interfaces page, IFACE_ROLE lines are written
# automatically and take priority over these two entries.
IFACE_WAN=eth0            # Interface facing your router / internet
IFACE_LAN=eth1            # Interface facing your local devices

# ── Interface role map (written by GUI, or set manually) ──────
# Roles: gateway  — primary WAN / default route (exactly one required)
#        wan      — additional WAN interface
#        lan      — LAN-side interface; multiple are bridged together
#        disabled — interface is ignored
IFACE_GATEWAY=eth0
# IFACE_ROLE=eth0:gateway
# IFACE_ROLE=eth1:lan
# IFACE_ROLE=eth2:disabled

# ── Router identity ───────────────────────────────────────────
ROUTER_IP=192.168.1.100   # The Pi's own IP on the LAN side

# ── Display mode ──────────────────────────────────────────────
enable_gui=true           # Set to false for headless / SSH operation

# ── QoS thresholds ────────────────────────────────────────────
ENABLE_ACCELERATION=true
LARGE_PACKET_THRESHOLD=1000   # Bytes — packets above this are "heavy"
PUNISH_TRIGGER_COUNT=30       # Heavy-packet hits before a flow is throttled
CLEANUP_INTERVAL=10000        # Flow-table cleanup interval (packets)

# ── Layer-2 bridge options ────────────────────────────────────
ENABLE_STP=false
ENABLE_IGMP_SNOOPING=false

# ── Built-in services ─────────────────────────────────────────
enable_nat=true
enable_dhcp=true
enable_dns_cache=true
enable_upnp=true
enable_firewall=true
enable_pppoe=false        # Enable for direct PPPoE ISP connections

# ── Per-device download caps (optional) ───────────────────────
# Format: IP_LIMIT=<device-IP>:<Mbps>
# IP_LIMIT=192.168.1.50:20
# IP_LIMIT=192.168.1.51:10
```

---

## Running

The program requires root to open raw network sockets and configure NIC hardware.

**GUI mode** (default — connect a monitor or use a VNC/framebuffer session):

```bash
cd build
sudo ./GamingTrafficPrioritizer
```

**Headless / CLI mode** (SSH, no display needed):

```bash
# In config.txt set:  enable_gui=false
cd build
sudo ./GamingTrafficPrioritizer
```

---

## Stopping

| Method | Result |
|--------|--------|
| GUI — click the **"关闭程序"** button | Clean shutdown; settings saved to `config.txt` |
| GUI — close the window (X button) | Same clean shutdown |
| CLI — press `Ctrl+C` | Sends SIGINT; all threads stop gracefully and settings are saved |

All three methods save your current configuration before exiting.

---

## Assigning Interface Roles (GUI)

Open the **Interfaces** page in the dashboard. Every network adapter detected on the system is listed in a table. Use the dropdown next to each interface to assign its role:

| Role | Meaning |
|------|---------|
| 默认网关 (Default Gateway) | Primary internet uplink — exactly one interface must hold this role |
| 外网 (WAN) | Additional internet-facing interface (multi-WAN) |
| 内网 (LAN) | Local device port — multiple LAN ports are bridged together |
| 禁用 (Disabled) | Interface is not used |

Click **保存并应用** to apply. Selecting a new default gateway automatically demotes the previous one to WAN. The role assignments are saved to `config.txt` on exit.

---

## Roadmap

- [x] Core QoS forwarding engine
- [x] Qt6 local dashboard (live charts, interface roles, QoS rules, service toggles)
- [ ] Remote web management dashboard (NGINX + C++ FastCGI REST API)
