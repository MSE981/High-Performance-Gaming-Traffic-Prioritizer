
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

### 1. Local GUI Frontend (Qt Dashboard)

We will build a local desktop application to display real-time network statistics on a monitor connected directly to the Raspberry Pi.

Technology Stack: Qt (C++).

Core Mechanism: To keep the interface highly responsive, the application will use Qt's native Signals and Slots mechanism .

Real-Time Charting: Drawing charts can easily freeze a program. To ensure the GUI never blocks the main thread, the fast-paced network data will be saved into a background Buffer using a Callback. Then, a safe Qt timerEvent will quietly read that buffer and refresh the screen at a steady, human-friendly frame rate.

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

libpcap-dev: Required by the CMake configuration for network packet handling.

### Future UI Dependencies 

libjsoncpp-dev: For the C++ FastCGI JSON parsing.

libfcgi-dev & nginx: For the web server backend.

qt6-base-dev: For the local GUI dashboard.

## 🔨 Build & Run Instructions

### 1. Install Dependencies ( Raspberry Pi OS ):


sudo apt update
sudo apt install build-essential cmake gcc-14 g++-14 ethtool speedtest-cli libgpiod-dev libpcap-dev

### 2. Compile the Project:


mkdir build && cd build
cmake ..
make -j4

### 3. Run the Engine:
Because the program needs to control physical network cards and manipulate hardware registers, it must be run with administrator privileges:


sudo ./GamingTrafficPrioritizer
