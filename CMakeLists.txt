cmake_minimum_required(VERSION 3.20)

option(USE_GCC14 "Force GCC 14 as C and C++ compiler (gcc-14 / g++-14)" OFF)
if(USE_GCC14)
    set(CMAKE_C_COMPILER "gcc-14")
    set(CMAKE_CXX_COMPILER "g++-14")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

project(GamingTrafficPrioritizer VERSION 3.0 LANGUAGES CXX)

include_directories(include)
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ── Raspberry Pi 5 (Cortex-A76) compile options ───────────────────────────────
set(PROJECT_ROUTER_COMPILE_OPTIONS "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    list(APPEND PROJECT_ROUTER_COMPILE_OPTIONS
        -O3
        -mcpu=cortex-a76
        -mtune=cortex-a76
        -pipe
        -Wall
        -Wextra
        -fexceptions
    )
elseif(MSVC)
    list(APPEND PROJECT_ROUTER_COMPILE_OPTIONS /std:c++latest /O2)
endif()

# ── Infrastructure utilities ──────────────────────────────────────────────────
add_library(utils_network    src/NetworkUtils.cpp src/NetworkTypes.cpp)
add_library(dataplane        src/DataPlane.cpp)
add_library(utils_system     src/SystemOptimizer.cpp)
add_library(config           src/Config.cpp)

# ── Data-plane engines ────────────────────────────────────────────────────────
add_library(engine_nat       src/NatEngine.cpp)
add_library(engine_dns       src/DnsEngine.cpp)
add_library(engine_dhcp      src/DhcpEngine.cpp)
add_library(engine_firewall  src/FirewallEngine.cpp)
add_library(engine_scheduler src/Scheduler.cpp)

target_link_libraries(engine_dns       PRIVATE dataplane)
target_link_libraries(engine_dhcp      PRIVATE dataplane utils_network)
target_link_libraries(engine_scheduler PRIVATE dataplane)

# ── I/O engines ───────────────────────────────────────────────────────────────
add_library(engine_net       src/NetworkEngine.cpp)
add_library(engine_upnp      src/UpnpEngine.cpp)

target_link_libraries(engine_upnp PRIVATE dataplane)

# ── Self-test component ───────────────────────────────────────────────────────
add_library(selftest         src/SelfTest.cpp)

# Apply compile options and _GNU_SOURCE to every library
set(PROJECT_ROUTER_STATIC_LIBS
    utils_network utils_system config dataplane
    engine_nat engine_dns engine_dhcp engine_firewall engine_scheduler
    engine_net engine_upnp
    selftest
)
foreach(lib IN LISTS PROJECT_ROUTER_STATIC_LIBS)
    target_compile_options(${lib} PRIVATE ${PROJECT_ROUTER_COMPILE_OPTIONS})
    target_compile_definitions(${lib} PRIVATE _GNU_SOURCE)
endforeach()

# ── Main application ──────────────────────────────────────────────────────────
add_executable(GamingTrafficPrioritizer
    src/main.cpp
    src/App.cpp
    src/Telemetry.cpp
    src/GUI/Dashboard.cpp
    include/GUI/Dashboard.hpp
)

target_link_libraries(GamingTrafficPrioritizer PRIVATE
    ${PROJECT_ROUTER_STATIC_LIBS}
    Qt6::Widgets
    Qt6::OpenGL
    Threads::Threads
)

target_compile_options(GamingTrafficPrioritizer PRIVATE ${PROJECT_ROUTER_COMPILE_OPTIONS})
target_compile_definitions(GamingTrafficPrioritizer PRIVATE _GNU_SOURCE)

# ── Demo programs (one executable per engine) ─────────────────────────────────
# Minimal link set per target (Ch.2 §2.1: one library boundary + client demo).
# With STATIC libraries, PRIVATE deps are not propagated to the executable;
# list dataplane explicitly for engines that use it.
if(EXISTS "${CMAKE_SOURCE_DIR}/demo")
    foreach(demo_src IN ITEMS
        demo/nat_demo.cpp
        demo/dns_demo.cpp
        demo/dhcp_demo.cpp
        demo/scheduler_demo.cpp
        demo/firewall_demo.cpp)
        if(EXISTS "${CMAKE_SOURCE_DIR}/${demo_src}")
            get_filename_component(demo_name ${demo_src} NAME_WE)
            add_executable(${demo_name} ${demo_src})
            if(demo_name STREQUAL "nat_demo")
                set(demo_link_libs engine_nat)
            elseif(demo_name STREQUAL "dns_demo")
                set(demo_link_libs engine_dns dataplane config)
            elseif(demo_name STREQUAL "dhcp_demo")
                set(demo_link_libs engine_dhcp dataplane utils_network)
            elseif(demo_name STREQUAL "scheduler_demo")
                set(demo_link_libs engine_scheduler dataplane)
            elseif(demo_name STREQUAL "firewall_demo")
                set(demo_link_libs engine_firewall config)
            else()
                message(FATAL_ERROR "demo ${demo_name}: add target_link_libs mapping")
            endif()
            target_link_libraries(${demo_name} PRIVATE ${demo_link_libs} Threads::Threads)
            target_compile_options(${demo_name} PRIVATE ${PROJECT_ROUTER_COMPILE_OPTIONS})
            target_compile_definitions(${demo_name} PRIVATE _GNU_SOURCE)
        endif()
    endforeach()
endif()

# Auto-privilege: disabled due to SSH non-interactive password.
# Run manually with: sudo ./GamingTrafficPrioritizer
