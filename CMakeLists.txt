cmake_minimum_required(VERSION 3.15)
project(tsduck-multiplexer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find required libraries
find_package(PkgConfig REQUIRED)

# Find TSDuck
pkg_check_modules(TSDUCK REQUIRED tsduck)

# Find yaml-cpp
find_package(yaml-cpp REQUIRED)

# Find Threads
find_package(Threads REQUIRED)

# Find CURL
find_package(CURL REQUIRED)

# Source files - Streamlined refactoring based on multi2 pattern
set(SOURCES
    src/main_new.cpp
    src/TCPReader.cpp
    src/TCPOutput.cpp
    src/StreamSplicer.cpp
    src/NALParser.cpp
)

# Create executable
add_executable(ts-multiplexer ${SOURCES})

# Include directories
target_include_directories(ts-multiplexer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${TSDUCK_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

# Library directories (needed for TSDuck libraries from pkg-config)
target_link_directories(ts-multiplexer PRIVATE
    ${TSDUCK_LIBRARY_DIRS}
)

# Link libraries
target_link_libraries(ts-multiplexer PRIVATE
    ${TSDUCK_LIBRARIES}
    Threads::Threads
)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ts-multiplexer PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# Installation
install(TARGETS ts-multiplexer DESTINATION bin)