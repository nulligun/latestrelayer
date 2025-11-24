#!/bin/bash
set -e

echo "=== TSDuck Multiplexer Entrypoint ==="

# Check if source files are mounted
if [ ! -d "/app/src" ]; then
    echo "ERROR: Source directory /app/src not found!"
    echo "Make sure to mount the source code volume in docker-compose.yml"
    exit 1
fi

if [ ! -f "/app/CMakeLists.txt" ]; then
    echo "ERROR: CMakeLists.txt not found!"
    echo "Make sure to mount CMakeLists.txt volume in docker-compose.yml"
    exit 1
fi

# Create build directory if it doesn't exist
mkdir -p /app/build
cd /app/build

# Check if we need to compile
NEED_COMPILE=0

# Check if binary exists
if [ ! -f "/usr/local/bin/ts-multiplexer" ]; then
    echo "Binary not found - compiling..."
    NEED_COMPILE=1
else
    # Check if any source file is newer than the binary
    if [ -n "$(find /app/src -name '*.cpp' -newer /usr/local/bin/ts-multiplexer)" ] || \
       [ -n "$(find /app/src -name '*.h' -newer /usr/local/bin/ts-multiplexer)" ] || \
       [ /app/CMakeLists.txt -nt /usr/local/bin/ts-multiplexer ]; then
        echo "Source files changed - recompiling..."
        NEED_COMPILE=1
    else
        echo "Binary is up to date - skipping compilation"
    fi
fi

# Compile if needed
if [ "$NEED_COMPILE" -eq 1 ]; then
    echo "Running CMake..."
    cmake .. || { echo "CMake failed!"; exit 1; }
    
    echo "Running Make..."
    make -j$(nproc) || { echo "Make failed!"; exit 1; }
    
    echo "Installing binary..."
    cp ts-multiplexer /usr/local/bin/ || { echo "Installation failed!"; exit 1; }
    
    echo "Compilation complete!"
else
    echo "Skipping compilation - binary is current"
fi

# Execute the CMD
echo "Starting multiplexer: $@"
exec "$@"