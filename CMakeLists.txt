cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_COMPILER "g++-14")
set(CMAKE_C_COMPILER "gcc-14")

project(GamingTrafficPrioritizer VERSION 3.0 LANGUAGES CXX)


include_directories(include)
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# 启用 Qt 自动元对象编译器 (MOC) 以支持信号与槽
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_executable(GamingTrafficPrioritizer 
    src/main.cpp
    src/GUI/Dashboard.cpp
    include/GUI/Dashboard.hpp
)

target_link_libraries(GamingTrafficPrioritizer PRIVATE 
    Qt6::Widgets
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

# 自动化授权：此步骤暂时由于 SSH 密码无终端交互而禁用。
# 手动运行时请使用 sudo ./GamingTrafficPrioritizer
