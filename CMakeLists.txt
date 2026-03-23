cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_COMPILER "g++-14")
set(CMAKE_C_COMPILER "gcc-14")

project(GamingTrafficPrioritizer VERSION 3.0 LANGUAGES CXX)


include_directories(include)
find_package(Threads REQUIRED)
find_library(PCAP_LIB pcap REQUIRED)

add_executable(GamingTrafficPrioritizer src/main.cpp)

target_link_libraries(GamingTrafficPrioritizer PRIVATE 
    ${PCAP_LIB}
    Threads::Threads
    gpiodcxx  
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)



# 针对树莓派 5 (Cortex-A76) 的极限优化
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(GamingTrafficPrioritizer PRIVATE 
        -std=c++23
        -O3
        -mcpu=cortex-a76
        -mtune=cortex-a76
        -pipe
        -Wall 
        -Wextra 
        -fexceptions
    )
elseif(MSVC)
    target_compile_options(GamingTrafficPrioritizer PRIVATE 
        /std:c++latest
    )
endif()

# 自动化授权：编译后自动赋予网络和实时调度权限
add_custom_command(TARGET GamingTrafficPrioritizer POST_BUILD
    COMMAND sudo /usr/sbin/setcap 'cap_net_raw,cap_net_admin,cap_sys_nice=eip' $<TARGET_FILE:GamingTrafficPrioritizer>
    COMMENT "Granting CAP_NET_RAW and Realtime permissions..."
)
