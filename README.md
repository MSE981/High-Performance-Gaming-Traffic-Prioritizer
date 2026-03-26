
# 🎮High-Performance Gaming Traffic Prioritizer

When multiple devices share a home network, heavy downloads or video streams can fill up the router's hardware buffer. This causes severe lag spikes and dropped connections for latency-sensitive applications like online multiplayer games.

The Gaming Traffic Prioritizer solves this problem at the lowest system level. Built purely in modern C++23 for the Raspberry Pi 5, this software acts as a transparent network bridge. It inspects every packet, identifies gaming and DNS traffic, and forwards them instantly. Meanwhile, heavy background downloads are placed into a strict software queue to prevent the physical network card from crashing.

The result: Zero lag, stable ping, and maximum bandwidth efficiency.

## ✨ Professional-Grade Features

🚀 Bare-Metal Speed: The engine bypasses the standard Linux network stack. It uses AF_PACKET raw sockets and low-level ioctl commands to talk directly to the network hardware.

🧠 Heuristic Traffic Analysis: It automatically tracks connection flows. By analyzing packet sizes and frequencies, it separates fast-paced gaming actions from heavy, bandwidth-consuming file downloads.

🚦 Three-Tier Priority System:

🟢Critical: DNS requests and TCP handshakes. These bypass all queues and are sent instantly to ensure web pages load immediately.

🟡High: Gaming traffic and secure web browsing . Sent with zero delay.

🔴Normal: Big downloads. These are sent to an 8192-slot memory buffer and released smoothly using a highly accurate "Token Bucket" algorithm.

💤 True Event-Driven Architecture: The software does not waste CPU power by constantly checking for new packets . It uses the poll() system call to sleep when the network is quiet and wakes up in a microsecond only when hardware interrupts occur.

🔒 Thread-Safe Memory Management: Each CPU core owns its specific network interface using std::unique_ptr. There are no shared mutable variables between the fast-path threads, guaranteeing total memory safety and zero lock-contention.

## 🚀 Future Roadmap: User Interface Development

The core forwarding engine is fully functional. The next phase of development focuses on building professional user interfaces, strictly following real-time embedded coding guidelines:

### 1. Local GUI Frontend (Qt6 Dashboard)

We have built a local desktop application to display real-time network statistics on a monitor connected directly to the Raspberry Pi.

**Technology Stack**: Qt 6 (C++).

**Strict Real-Time Adherence**:
- **Pure C++ Layouts**: Zero reliance on XML or `.ui` files. The layout is explicitly declared using nested `QVBoxLayout` and `QHBoxLayout` arrays to ensure memory transparency and comply with modern declarative design.
- **Signals and Slots**: All callbacks execute instantly via pointer-based `connect()` functions. Heavy background operations are detached to independent `std::jthread` contexts, guaranteeing zero latency spikes in the GUI.
- **Data Buffering vs Screen Refresh**: Due to the massive hardware packet rates, the GUI never samples network traffic instantaneously. Differential calculation for Packets Per Second (PPS) and Bytes Per Second (BPS) is pushed to a designated `Shift Buffer` asynchronously. 
- **Throttled Updates (25Hz)**: To conserve CPU, visual updates strictly respect a `QTimer` driven limit of 40ms.
- **Timer Restrictions**: Qt's timing resolution is banned for true data sampling. The global `QTimer` dictates *screen refreshes only*; network probing remains solely governed by kernel `timerfd_create` logic mapping to `Core 1/2/3`.

**Cyber-Physical Motion Engine**:
- The UI mimics fluid, high-end "iOS 25" style interactions via an integrated **RK4 (Runge-Kutta) Spring-Damper Engine**. Chart axis fluctuations absorb bursts natively without visually aggressive jitter.
- Rendered using **Dual-Pass Liquid Glass Glow**, an overlapping path algorithm simulating diffused neon refractions beneath a solid vector core wireframe.

### 2. Remote Web Management Dashboard

To allow users to control the prioritizer from a phone or laptop, we are building a high-performance web interface.

Web Server: NGINX.

Server-Side Technology : FastCGI written in C++. We will strictly avoid standard web languages like PHP or Node.js. NGINX will communicate with our C++ FastCGI program through a Unix Socket for maximum speed.

Protocol & Data Format: The backend will provide a clean REST API. We will use the jsoncpp library in C++ to generate and parse JSON data efficiently.

Client-Side Technology : Pure HTML and JavaScript . The webpage will operate like a modern application. When a user interacts with the page, JavaScript will use $.getJSON or POST requests to asynchronously ask the C++ FastCGI server for data. Once the JSON arrives, the JavaScript will only update the specific parts of the webpage that changed, without ever reloading the entire page.

## ⚙️ System Requirements

To build and run this project, your system must meet the following hardware and software requirements:

### Hardware

Raspberry Pi 5 .

A secondary USB Ethernet Adapter .

### Build Tools & Compilers

GCC 14 .

CMake 3.20 or higher.

### Required Runtime Dependencies

ethtool: Required to disable hardware offloads (GRO/TSO) to prevent giant merged packets.

speedtest-cli: Required by the automated Probe module to measure your true ISP bandwidth asynchronously.

libgpiod-dev (v2): Required for the zero-latency memory-mapped LED status indicators.

qt6-base-dev: Required for compiling the local GUI dashboard and eglfs direct rendering.

### Future UI Dependencies 

libjsoncpp-dev: For the C++ FastCGI JSON parsing.

libfcgi-dev & nginx: For the web server backend.

## 🔨 Build & Run Instructions

### 1. Install Dependencies (Raspberry Pi OS)

```bash
sudo apt update
sudo apt install build-essential cmake gcc-14 g++-14 \
    libgpiod-dev libgpiod2 \
    qt6-base-dev qt6-base-dev-tools \
    ethtool speedtest-cli
```

### 2. Identify Your Network Interfaces

The program requires two separate Ethernet interfaces: one connected to your router (WAN) and one connected to your devices (LAN).

```bash
ip link show
```

Note the interface names (e.g. `eth0` for WAN, `eth1` for LAN). You will need them in the next step.

### 3. Create a Configuration File

Create a `config.txt` file in the same directory as the binary. All fields are optional — the program runs on defaults if the file is absent.

```ini
# Network interfaces
IFACE_WAN=eth0            # Interface connected to your upstream router
IFACE_LAN=eth1            # Interface connected to your LAN / devices

# Router's own LAN IP address (used by NAT, DHCP, UPnP)
ROUTER_IP=192.168.1.100

# Set to false to run in headless CLI mode (no Qt window)
enable_gui=true

# Acceleration mode: true = heuristic QoS active, false = transparent bridge
ENABLE_ACCELERATION=true

# Heuristic detection thresholds
LARGE_PACKET_THRESHOLD=1000   # Bytes; packets above this count as "large"
PUNISH_TRIGGER_COUNT=30       # Large-packet hits before a flow is deprioritised
CLEANUP_INTERVAL=10000        # Flow-table cleanup interval (packets)

# Bridge / Layer-2 options
ENABLE_STP=false              # Enable Spanning Tree Protocol on the bridge
ENABLE_IGMP_SNOOPING=false    # Enable IGMP snooping for multicast filtering
# BRIDGE_IFACE=eth0           # Interfaces to add to the bridge (repeat for each)
# BRIDGE_IFACE=eth1

# Feature toggles (persisted across restarts by the GUI settings page)
enable_nat=true               # Network Address Translation
enable_dhcp=true              # Built-in DHCP server
enable_dns_cache=true         # In-process DNS cache
enable_upnp=true              # UPnP/IGD port-mapping daemon
enable_firewall=true          # Stateful packet filter
enable_pppoe=false            # PPPoE client (for direct ISP connections)

# Per-device download rate caps (optional, one line per device)
# Format: IP_LIMIT=<IP>:<Mbps>
# IP_LIMIT=192.168.1.50:20
# IP_LIMIT=192.168.1.51:10
```

> **Auto-save**: On every clean exit (GUI close button, window X, or `Ctrl+C`), the program automatically overwrites `config.txt` with the current runtime state. Any changes made through the GUI settings page are therefore persisted automatically — no manual editing required.

### 4. Build

```bash
mkdir build && cd build
cmake ..
cmake --build . -j4
```

The compiled binary `GamingTrafficPrioritizer` will be placed inside the `build/` directory.

### 5. Run

The program must run with root privileges because it opens raw AF_PACKET sockets and modifies hardware NIC registers directly.

**GUI mode** (default, requires a connected display):

```bash
sudo ./GamingTrafficPrioritizer
```

**Headless / CLI mode** (no display required, e.g. SSH):

```bash
# Set enable_gui=false in config.txt, then:
sudo ./GamingTrafficPrioritizer
```

### 6. Shutdown

| Method | Description |
|--------|-------------|
| GUI — "关闭程序" button | Red button in the top header bar. Triggers a clean shutdown: all worker threads are stopped and joined before the process exits. |
| GUI — Window X button | Same clean shutdown path as above. |
| CLI — `Ctrl+C` | Sends `SIGINT`. The signal handler calls `app.stop()`, unblocking all threads for a graceful exit. |

> All three paths converge on the same shutdown sequence: `app.stop()` sets the internal promise, `std::jthread` destructors request stop tokens and join each worker thread, `Config::save_config()` persists the current runtime state to `config.txt`, then the process exits cleanly.
