cmake_minimum_required(VERSION 3.16)
project(ts_loop_to_rtmp VERSION 1.0.0 LANGUAGES CXX)

# Set C++20 standard (required by TSDuck)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable warnings
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(TSDUCK REQUIRED tsduck)

# Find Threads
find_package(Threads REQUIRED)

# Source files
set(SOURCES
    src/main.cpp
)

set(TCP_SOURCES
    src/tcp_main.cpp
)

# Create executables
add_executable(ts_loop_to_rtmp ${SOURCES})
add_executable(ts_tcp_splicer ${TCP_SOURCES})

# Include directories
target_include_directories(ts_loop_to_rtmp PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${TSDUCK_INCLUDE_DIRS}
)

target_include_directories(ts_tcp_splicer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${TSDUCK_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(ts_loop_to_rtmp PRIVATE
    ${TSDUCK_LIBRARIES}
    Threads::Threads
)

target_link_libraries(ts_tcp_splicer PRIVATE
    ${TSDUCK_LIBRARIES}
    Threads::Threads
)

# Link directories
target_link_directories(ts_loop_to_rtmp PRIVATE
    ${TSDUCK_LIBRARY_DIRS}
)

target_link_directories(ts_tcp_splicer PRIVATE
    ${TSDUCK_LIBRARY_DIRS}
)

# Compiler options from TSDuck pkg-config
target_compile_options(ts_loop_to_rtmp PRIVATE
    ${TSDUCK_CFLAGS_OTHER}
)

target_compile_options(ts_tcp_splicer PRIVATE
    ${TSDUCK_CFLAGS_OTHER}
)

# Install targets
install(TARGETS ts_loop_to_rtmp ts_tcp_splicer
    RUNTIME DESTINATION bin
)

# Print configuration
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "TSDuck include dirs: ${TSDUCK_INCLUDE_DIRS}")
message(STATUS "TSDuck library dirs: ${TSDUCK_LIBRARY_DIRS}")
message(STATUS "TSDuck libraries: ${TSDUCK_LIBRARIES}")