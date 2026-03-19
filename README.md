High-Performance Gaming Traffic Prioritizer


Project Introduction

GamingTrafficPrioritizer is a high-performance network engine designed for the Raspberry Pi 4B. Developed using the latest C++23 standard, this tool acts as a transparent bridge to manage and optimize home network traffic in real-time.
The primary goal of the project is to eliminate "Bufferbloat"—the lag caused when large downloads or video streams slow down your connection. By sitting between your router and gaming devices, it analyzes every data packet and ensures that critical gaming information is always sent first. This provides a stable, low-latency experience for competitive gaming even when the network is under heavy load.


Core Features

Zero-Copy Forwarding Engine: Uses Linux kernel mmap  technology to move data directly between network interfaces. This minimizes CPU usage and reduces processing time to microseconds by avoiding slow data copying.
Heuristic Traffic Fingerprinting: A smart identification system that analyzes the history of data flows. It can tell the difference between a real game and a download program trying to "disguise" itself as a game by using specific network ports.
TCP ACK Acceleration: Automatically identifies and prioritizes small "Acknowledgement" packets. By speeding up these feedback signals, it prevents heavy downloads from blocking the path of your gaming data.


Hardware-Level Optimization:

CPU Pinning: Locks critical tasks to specific CPU cores (Core 2 & 3) to prevent system interruptions.
Frequency Locking: Forces the Raspberry Pi CPU to stay at Performance Mode to ensure predictable performance.
Environmental Awareness (Probes): On startup, the system automatically tests the speed limits of your hardware and your Internet Service Provider. This data allows the engine to set the perfect balance for your specific network environment.
Real-Time Hardware Feedback: Includes a built-in Watchdog system and RGB LED support to provide instant visual updates on system health and network load levels.
