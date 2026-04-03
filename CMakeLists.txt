cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_COMPILER "g++-14")
set(CMAKE_C_COMPILER "gcc-14")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

project(GamingTrafficPrioritizer VERSION 3.0 LANGUAGES CXX)


include_directories(include)
find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL)

# Enable Qt Meta-Object Compiler (MOC) for signals and slots
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_executable(GamingTrafficPrioritizer
    src/main.cpp
    src/GUI/Dashboard.cpp
    include/GUI/Dashboard.hpp
)

target_link_libraries(GamingTrafficPrioritizer PRIVATE
    Qt6::Widgets
    Qt6::OpenGL
    Threads::Threads
)

target_compile_definitions(GamingTrafficPrioritizer PRIVATE _GNU_SOURCE)



# Raspberry Pi 5 (Cortex-A76) peak optimization flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(GamingTrafficPrioritizer PRIVATE
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

# Auto-privilege: disabled due to SSH non-interactive password.
# Run manually with: sudo ./GamingTrafficPrioritizer
