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

# ----- Raspberry Pi 4 (Cortex-A72) compile options -----
set(PROJECT_ROUTER_COMPILE_OPTIONS "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    list(APPEND PROJECT_ROUTER_COMPILE_OPTIONS
        -O3
        -mcpu=cortex-a72
        -mtune=cortex-a72
        -pipe
        -Wall
        -Wextra
        -fexceptions
    )
elseif(MSVC)
    list(APPEND PROJECT_ROUTER_COMPILE_OPTIONS /std:c++latest /O2)
endif()

# ----- Infrastructure utilities -----
add_library(utils_network    src/Network_util.cpp src/Types_util.cpp)
add_library(dataplane        src/TxFrameOutput_util.cpp)
add_library(utils_system     src/SystemOptimizer_util.cpp)
add_library(config           src/Config.cpp)

# ----- Data-plane engines -----
add_library(engine_nat       src/NatEngine.cpp)
add_library(engine_dhcp      src/DhcpEngine.cpp)
add_library(engine_scheduler src/Scheduler_util.cpp)
add_library(forwarding       src/ForwardingState_util.cpp)

target_link_libraries(engine_dhcp      PRIVATE dataplane utils_network)
target_link_libraries(engine_scheduler PRIVATE dataplane)
target_link_libraries(forwarding        PRIVATE utils_network config)

# ----- I/O engines -----
add_library(rawsocket_util  src/RawSocket_util.cpp)

# ----- Self-test component -----
add_library(selftest         src/SelfTest.cpp)

# Apply compile options and _GNU_SOURCE to every library
set(PROJECT_ROUTER_STATIC_LIBS
    utils_network utils_system config dataplane
    engine_nat engine_dhcp engine_scheduler forwarding
    rawsocket_util
    selftest
)
foreach(lib IN LISTS PROJECT_ROUTER_STATIC_LIBS)
    target_compile_options(${lib} PRIVATE ${PROJECT_ROUTER_COMPILE_OPTIONS})
    target_compile_definitions(${lib} PRIVATE _GNU_SOURCE)
endforeach()

# ----- Main application -----
add_executable(GamingTrafficPrioritizer
    src/main.cpp
    src/App.cpp
    src/ControlPlane.cpp
    src/NetworkConfig.cpp
    src/ParameterServices.cpp
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

# Auto-privilege: disabled due to SSH non-interactive password.
# Run manually with: sudo ./GamingTrafficPrioritizer
