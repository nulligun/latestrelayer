#!/bin/bash
set -e

echo "=== TSDuck Multiplexer Entrypoint ==="

# Signal handler for graceful shutdown during setup
cleanup() {
    echo "[Entrypoint] Received shutdown signal during setup"
    exit 0
}

# Trap SIGTERM and SIGINT during compilation phase
trap cleanup SIGTERM SIGINT

# TSDuck cache directory (mounted from host)
TSDUCK_CACHE="/opt/tsduck-cache"

# Function to check if TSDuck is properly installed in the system
check_tsduck_system() {
    [ -f "/usr/bin/tsp" ] && [ -f "/usr/lib/libtsduck.so" ]
}

# Function to check if TSDuck is cached on host
check_tsduck_cache() {
    [ -f "$TSDUCK_CACHE/bin/tsp" ] && [ -f "$TSDUCK_CACHE/lib/libtsduck.so" ]
}

# Function to install TSDuck from cache to system
install_tsduck_from_cache() {
    echo "[tsduck] Installing TSDuck from host cache..."
    cp -a "$TSDUCK_CACHE/bin/"* /usr/bin/
    cp -a "$TSDUCK_CACHE/lib/"* /usr/lib/
    if [ -d "$TSDUCK_CACHE/include" ]; then
        cp -a "$TSDUCK_CACHE/include/"* /usr/include/
    fi
    if [ -d "$TSDUCK_CACHE/share" ]; then
        cp -a "$TSDUCK_CACHE/share/"* /usr/share/
    fi
    ldconfig
    echo "[tsduck] TSDuck installed from cache successfully"
}

# Function to export TSDuck from Docker layer to cache
export_tsduck_to_cache() {
    echo "[tsduck] Exporting TSDuck to host cache for future use..."
    mkdir -p "$TSDUCK_CACHE/bin" "$TSDUCK_CACHE/lib" "$TSDUCK_CACHE/include" "$TSDUCK_CACHE/share"
    
    # Copy binaries
    cp -a /usr/bin/ts* "$TSDUCK_CACHE/bin/" 2>/dev/null || true
    
    # Copy libraries
    cp -a /usr/lib/libtsduck.so "$TSDUCK_CACHE/lib/" 2>/dev/null || true
    cp -a /usr/lib/libtscore.so "$TSDUCK_CACHE/lib/" 2>/dev/null || true
    cp -a /usr/lib/tsduck "$TSDUCK_CACHE/lib/" 2>/dev/null || true
    
    # Copy headers
    cp -a /usr/include/tsduck "$TSDUCK_CACHE/include/" 2>/dev/null || true
    cp -a /usr/include/tscore "$TSDUCK_CACHE/include/" 2>/dev/null || true
    
    # Copy share files (pkgconfig, etc.)
    cp -a /usr/share/tsduck "$TSDUCK_CACHE/share/" 2>/dev/null || true
    cp -a /usr/share/pkgconfig/tsduck.pc "$TSDUCK_CACHE/share/" 2>/dev/null || true
    cp -a /usr/share/pkgconfig/tscore.pc "$TSDUCK_CACHE/share/" 2>/dev/null || true
    
    echo "[tsduck] TSDuck exported to cache: $TSDUCK_CACHE"
}

# Handle TSDuck installation/caching
if [ -d "$TSDUCK_CACHE" ]; then
    if check_tsduck_cache; then
        echo "[tsduck] Found TSDuck in host cache"
        install_tsduck_from_cache
    elif check_tsduck_system; then
        echo "[tsduck] TSDuck found in Docker layer, exporting to cache..."
        export_tsduck_to_cache
    else
        echo "[tsduck] WARNING: No TSDuck found in cache or Docker layer!"
    fi
else
    echo "[tsduck] Cache directory not mounted, using Docker layer TSDuck"
fi

# Verify TSDuck is available
if ! check_tsduck_system; then
    echo "ERROR: TSDuck is not properly installed!"
    exit 1
fi
echo "[tsduck] TSDuck version: $(tsp --version 2>&1 | head -1)"

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

# Execute the CMD (exec replaces shell with process, signals go directly to it)
echo "Starting multiplexer: $@"
# Remove trap since exec will replace this shell with the target process
trap - SIGTERM SIGINT
exec "$@"