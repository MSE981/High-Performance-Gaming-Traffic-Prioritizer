# Scalpel — Gaming Traffic Prioritizer

> **A Raspberry Pi 5 that sits between your router and your desk. Your games get the fast lane. Everything else waits its turn.**

Built in C++23 on Linux real-time primitives — no iptables magic, no kernel modules, no black boxes. Just raw-socket packet inspection, lock-free data structures, and a transparent bridge that your network devices never even notice.

---

## Why does this exist?

You're mid-ranked in a competitive match. Ping is 12 ms. Then someone on your network starts a 50 GB game update.

Ping jumps to 180 ms. You lose. You blame the ISP.

Scalpel solves this at the **hardware level** — not with QoS rules you have to maintain, not with router firmware you have to flash, but with a small Pi sitting quietly in your Ethernet chain, reading every packet and making routing decisions in microseconds.

---

## How it works

```
                        ┌──────────────────────────────────────┐
Internet ──── Router ──▶│ eth0  [Scalpel Pi 5]  eth1 ──▶ Switch / devices
                        │                                      │
                        │  Core 0 ▸ Qt dashboard               │
                        │  Core 1 ▸ Watchdog · NAT · DHCP      │
                        │  Core 2 ▸ WAN → LAN  (download)      │
                        │  Core 3 ▸ LAN → WAN  (upload)        │
                        └──────────────────────────────────────┘
```

Every packet is classified into one of three lanes before it leaves:

| Lane | What goes here | Treatment |
|------|---------------|-----------|
| **Critical** | DNS queries, TCP handshakes | Bypasses all queues — out immediately |
| **High** | Gaming packets, small UDP bursts | Zero-wait forwarding |
| **Normal** | Large downloads, video streams | Rate-controlled token bucket |

The classifier is heuristic — it watches flow sizes and packet frequencies to detect heavy traffic automatically. No port lists to maintain for most games.

---

## Feature highlights

- **Zero-copy forwarding** — kernel ring buffer (`TPACKET` mmap) means packet data is never copied between kernel and userspace
- **Lock-free hot path** — FNV-1a static hash tables; no `std::unordered_map`, no heap allocation while packets are flying
- **Core-pinned threads** — each CPU core has one job and stays on it (`SCHED_FIFO`, `pthread_setaffinity`)
- **Real-time timing** — `timerfd` + blocking `read()` drives all periodic work; no `sleep_for` anywhere in the forwarding path
- **Per-device bandwidth caps** — token-bucket rate limiter per IP, updated lock-free via RCU double-buffer swap
- **Full service stack** — NAT (SNAT/DNAT), DHCP server, DNS cache, UPnP/IGD, stateful firewall; each togglable at runtime
- **Qt6 live dashboard** — real-time packet rate / bandwidth charts, per-core stats, interface roles, QoS rules, service controls; rendered via Qt eglfs (direct DRM, no compositor) on a DSI display with GPU acceleration
- **Adaptive-rate GUI** — 60 Hz vsync-locked animation during interactions (notification pull-down, transitions), dropping to 1 Hz data-only refresh when idle; zero GPU work when the screen is static
- **iOS-style notification centre** — pull-down panel with spring-physics animation; engine alerts surface here without interrupting the forwarding path
- **Headless / SSH mode** — `enable_gui=false` and you never need a monitor

---

## Requirements

### Hardware

| Item | Notes |
|------|-------|
| Raspberry Pi 5 | Any RAM variant; 4 GB recommended |
| Second Ethernet adapter | USB 3.0 GbE adapter works great as LAN port |
| DSI display (optional) | Waveshare 8.0" 1280×800 or compatible; needed for GUI mode |

Plug it in like this:

```
Internet ──── Router ──── [Pi eth0 | Pi eth1] ──── Your switch / devices
```

If you only have one device, plug it directly into `eth1`. Done.

### Software

| Package | Why |
|---------|-----|
| GCC 14 + CMake 3.20+ | C++23 required (`std::expected`, `std::jthread`, `std::print`) |
| `qt6-base-dev` | Local dashboard |
| `qt6-qpa-plugins` | Qt6 eglfs platform plugin — drives DSI display directly via DRM, no compositor needed |
| `ethtool` | Strips hardware offloads that break raw-socket packet lengths |
| `speedtest-cli` | Optional ISP bandwidth auto-measurement (Probe C) |

---

## Build & install

```bash
# 1. Clone
git clone <repo-url>
cd High-Performance-Gaming-Traffic-Prioritizer

# 2. Build & run (start.sh installs dependencies, builds, and launches automatically)
sudo ./start.sh

# Or headless (no display needed)
# Set enable_gui=false in config/config.txt, then:
cd build && sudo ./GamingTrafficPrioritizer
```

The binary lands in `build/`. No install step needed — just copy it wherever you want alongside `config.txt`.

---

## Configuration

Drop a `config.txt` next to the binary. All keys are optional — Scalpel starts with sensible defaults if the file is absent. The GUI writes changes back on every clean exit.

```ini
# ── Network interfaces ────────────────────────────────────────────────
IFACE_WAN=eth0           # Faces your router (internet side)
IFACE_LAN=eth1           # Faces your devices (local side)
ROUTER_IP=192.168.1.100  # Pi's own LAN IP (used by DHCP/NAT)

# ── Interface role map ────────────────────────────────────────────────
# The GUI Interfaces page writes these automatically.
# Roles: gateway | wan | lan | disabled
IFACE_GATEWAY=eth0
# IFACE_ROLE=eth0:gateway
# IFACE_ROLE=eth1:lan

# ── Display mode ──────────────────────────────────────────────────────
enable_gui=true          # false → headless/SSH mode, no Qt dependency at runtime

# ── Traffic classification tuning ────────────────────────────────────
ENABLE_ACCELERATION=true
LARGE_PACKET_THRESHOLD=1000   # Packets above this byte count are "heavy"
PUNISH_TRIGGER_COUNT=30       # Heavy-packet count before a flow is throttled
CLEANUP_INTERVAL=10000        # Flow table sweep interval (packets)

# ── Built-in services ────────────────────────────────────────────────
enable_nat=true
enable_dhcp=true
enable_dns_cache=true
enable_upnp=true
enable_firewall=true
enable_pppoe=false       # Direct PPPoE ISP connection

# ── Per-device download caps ─────────────────────────────────────────
# IP_LIMIT=192.168.1.50:20     # 20 Mbps cap for this device
# IP_LIMIT=192.168.1.51:5      # 5 Mbps cap (your housemate's PC during your raid)

# ── Layer-2 bridge options ───────────────────────────────────────────
ENABLE_STP=false
ENABLE_IGMP_SNOOPING=false
```

---

## Running

```bash
# GUI mode — eglfs drives the DSI display directly via DRM, no compositor needed
sudo ./start.sh

# Headless / SSH — set enable_gui=false in config/config.txt
cd build && sudo ./GamingTrafficPrioritizer
```

`start.sh` installs missing dependencies, rebuilds if sources have changed, strips ethtool offloads on both interfaces, and launches the binary with the correct eglfs environment. The GUI renders at 60 Hz during animations and drops to 1 Hz while idle, keeping Core 0 free for OS scheduling.

### Stopping cleanly

| Method | What happens |
|--------|-------------|
| Dashboard → **Shutdown** button | Graceful stop, config saved |
| Close the window (× button) | Same graceful stop |
| `Ctrl+C` in terminal (CLI mode) | SIGINT caught, all threads join, config saved |

All three paths write `config.txt` before the process exits.

---

## Architecture deep-dive

If you're curious about what's happening under the hood:

**Forwarding pipeline (Cores 2 & 3)**

Each core runs a `poll()` loop on a `TPACKET`-mapped raw socket. When a packet arrives, it passes through a compile-time-assembled pipeline of function pointers (no virtual dispatch, no `if`/`else` chains at runtime):

```
[DHCP intercept] → [DNS intercept] → [NAT] → [per-IP shaper] → [QoS route]
```

The QoS route step dispatches through a 2×3 function pointer table (`route_mode × priority`) — a single array lookup, zero branches.

**Flow tracking**

The heuristic classifier uses a `StaticFlowMap<4096>` — a fixed-size open-addressing hash table with FNV-1a hashing and linear probing. Pre-allocated at construction, zero allocations per packet.

**Watchdog (Core 1)**

Runs on a 1-second `timerfd`. Refreshes CPU temperature and system info via raw `open()/read()/close()` — no `ifstream`, no heap objects. Drives NAT session cleanup, DNS TTL expiry, and DHCP lease management.

**RCU config updates**

The per-IP rate-limit table uses a lock-free double-buffer: the control plane writes to the inactive buffer and flips an `atomic<size_t>` index; the forwarding cores load the index with `memory_order_acquire` and read whichever buffer is currently active. No locks, no stalls.

---

## Roadmap

- [x] Core QoS forwarding engine (3-tier, heuristic flow classification)
- [x] Token-bucket rate limiter with per-IP caps
- [x] Built-in NAT, DHCP, DNS cache, UPnP, firewall
- [x] Qt6 local dashboard — live charts, role assignment, service toggles
- [x] Headless CLI mode
- [x] eglfs kiosk mode — GPU-accelerated fullscreen GUI on DSI via direct DRM, no compositor or desktop environment
- [x] Adaptive frame rate — 60 Hz animations, 1 Hz idle; DRM vblank-synced via Qt eglfs
- [x] iOS-style notification centre with spring-physics pull-down panel
- [ ] Remote web management (NGINX + C++ FastCGI REST API)
- [ ] IPv6 forwarding support
- [ ] WireGuard VPN integration

---

## License

MIT — do whatever you want with it.
