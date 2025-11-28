#!/bin/bash
set -e

echo "=== TSDuck Multiplexer Entrypoint ==="
echo "All build artifacts stored on host-mounted volumes"

# Signal handler for graceful shutdown during setup
cleanup() {
    echo "[Entrypoint] Received shutdown signal during setup"
    exit 0
}

# Trap SIGTERM and SIGINT during compilation phase
trap cleanup SIGTERM SIGINT

# Host-mounted directories
TSDUCK_SRC="/opt/tsduck-src"
TSDUCK_BUILD="/opt/tsduck-build"
TSDUCK_INSTALL="/opt/tsduck"
MULTIPLEXER_BUILD="/app/build"

# TSDuck build options (same as original Dockerfile)
TSDUCK_MAKE_OPTS="NOTELETEXT=1 NOSRT=1 NORIST=1 NODTAPI=1 NOVATEK=1 NOWARNING=1 CXXFLAGS_EXTRA=-Wno-error=attributes"

# ============================================================================
# TSDuck Source Management
# ============================================================================

clone_tsduck_source() {
    echo "[tsduck] Cloning TSDuck source to host mount..."
    rm -rf "$TSDUCK_SRC"/*
    git clone --depth=1 https://github.com/tsduck/tsduck.git "$TSDUCK_SRC"
    
    # Remove problem NIP files (same as original)
    cd "$TSDUCK_SRC"
    rm -rf src/libtsduck/dtv/dvbnip
    find src/tsplugins -name "*nip*" -delete 2>/dev/null || true
    find src/tstools -name "*nip*" -delete 2>/dev/null || true
    
    # Mark the source with a timestamp
    date +%s > "$TSDUCK_SRC/.clone_timestamp"
    echo "[tsduck] TSDuck source cloned successfully"
}

check_tsduck_source() {
    # Check if TSDuck source exists and has key files
    [ -d "$TSDUCK_SRC/src" ] && [ -f "$TSDUCK_SRC/Makefile" ]
}

# ============================================================================
# TSDuck Build Management
# ============================================================================

build_tsduck() {
    echo "[tsduck] Building TSDuck from source..."
    echo "[tsduck] This will take ~35 minutes on first run"
    
    cd "$TSDUCK_SRC"
    
    # Build TSDuck
    echo "[tsduck] Running make..."
    make -j$(nproc) $TSDUCK_MAKE_OPTS
    
    # Create timestamp marker for the build
    date +%s > "$TSDUCK_BUILD/.build_timestamp"
    
    echo "[tsduck] TSDuck build complete"
}

install_tsduck() {
    echo "[tsduck] Installing TSDuck to host mount..."
    
    cd "$TSDUCK_SRC"
    
    # Create install directories
    mkdir -p "$TSDUCK_INSTALL/bin"
    mkdir -p "$TSDUCK_INSTALL/lib/tsduck"
    mkdir -p "$TSDUCK_INSTALL/include/tsduck"
    mkdir -p "$TSDUCK_INSTALL/include/tscore"
    mkdir -p "$TSDUCK_INSTALL/share/tsduck"
    
    # Copy binaries
    cp -a bin/release-x86_64-linux-gnu/ts* "$TSDUCK_INSTALL/bin/" 2>/dev/null || \
    cp -a bin/release-*/ts* "$TSDUCK_INSTALL/bin/" 2>/dev/null || true
    
    # Copy libraries
    cp -a bin/release-x86_64-linux-gnu/libtsduck.so "$TSDUCK_INSTALL/lib/" 2>/dev/null || \
    cp -a bin/release-*/libtsduck.so "$TSDUCK_INSTALL/lib/" 2>/dev/null || true
    cp -a bin/release-x86_64-linux-gnu/libtscore.so "$TSDUCK_INSTALL/lib/" 2>/dev/null || \
    cp -a bin/release-*/libtscore.so "$TSDUCK_INSTALL/lib/" 2>/dev/null || true
    
    # Copy plugins
    cp -a bin/release-x86_64-linux-gnu/tsplugins/*.so "$TSDUCK_INSTALL/lib/tsduck/" 2>/dev/null || \
    cp -a bin/release-*/tsplugins/*.so "$TSDUCK_INSTALL/lib/tsduck/" 2>/dev/null || true
    
    # Copy headers
    cp -a src/libtsduck/*.h "$TSDUCK_INSTALL/include/tsduck/" 2>/dev/null || true
    cp -a src/libtsduck/**/*.h "$TSDUCK_INSTALL/include/tsduck/" 2>/dev/null || true
    cp -a src/libtscore/*.h "$TSDUCK_INSTALL/include/tscore/" 2>/dev/null || true
    
    # Create pkgconfig directory and files
    mkdir -p "$TSDUCK_INSTALL/lib/pkgconfig"
    
    cat > "$TSDUCK_INSTALL/lib/pkgconfig/tsduck.pc" << 'PKGCONFIG'
prefix=/opt/tsduck
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Name: tsduck
Description: TSDuck MPEG Transport Stream Toolkit
Version: 3.x
Libs: -L${libdir} -ltsduck -ltscore
Cflags: -I${includedir}/tsduck -I${includedir}/tscore
PKGCONFIG

    cat > "$TSDUCK_INSTALL/lib/pkgconfig/tscore.pc" << 'PKGCONFIG'
prefix=/opt/tsduck
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Name: tscore
Description: TSDuck Core Library
Version: 3.x
Libs: -L${libdir} -ltscore
Cflags: -I${includedir}/tscore
PKGCONFIG
    
    # Create timestamp marker
    date +%s > "$TSDUCK_INSTALL/.install_timestamp"
    
    echo "[tsduck] TSDuck installed to $TSDUCK_INSTALL"
}

check_tsduck_installed() {
    # Check if TSDuck is properly installed on host mount
    [ -f "$TSDUCK_INSTALL/bin/tsp" ] && [ -f "$TSDUCK_INSTALL/lib/libtsduck.so" ]
}

setup_tsduck_environment() {
    # Add TSDuck to PATH and library path
    export PATH="$TSDUCK_INSTALL/bin:$PATH"
    export LD_LIBRARY_PATH="$TSDUCK_INSTALL/lib:$TSDUCK_INSTALL/lib/tsduck:${LD_LIBRARY_PATH:-}"
    export PKG_CONFIG_PATH="$TSDUCK_INSTALL/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export TSDUCK_INSTALL_PREFIX="$TSDUCK_INSTALL"
    
    # Update library cache for this session
    ldconfig "$TSDUCK_INSTALL/lib" 2>/dev/null || true
    
    echo "[tsduck] Environment configured:"
    echo "  PATH includes: $TSDUCK_INSTALL/bin"
    echo "  LD_LIBRARY_PATH includes: $TSDUCK_INSTALL/lib"
}

# ============================================================================
# TSDuck Orchestration
# ============================================================================

ensure_tsduck() {
    echo "[tsduck] Checking TSDuck installation..."
    
    # Check if already installed
    if check_tsduck_installed; then
        echo "[tsduck] TSDuck found in host mount: $TSDUCK_INSTALL"
        setup_tsduck_environment
        echo "[tsduck] TSDuck version: $(tsp --version 2>&1 | head -1 || echo 'unknown')"
        return 0
    fi
    
    echo "[tsduck] TSDuck not found, need to build..."
    
    # Check if source exists
    if ! check_tsduck_source; then
        echo "[tsduck] Source not found, cloning..."
        clone_tsduck_source
    else
        echo "[tsduck] Source found in host mount: $TSDUCK_SRC"
    fi
    
    # Build TSDuck
    build_tsduck
    
    # Install to host mount
    install_tsduck
    
    # Setup environment
    setup_tsduck_environment
    
    echo "[tsduck] TSDuck version: $(tsp --version 2>&1 | head -1 || echo 'unknown')"
}

# ============================================================================
# Multiplexer Compilation (same as before, but uses host-mounted TSDuck)
# ============================================================================

compile_multiplexer() {
    echo "[multiplexer] Checking compilation status..."
    
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
    mkdir -p "$MULTIPLEXER_BUILD"
    cd "$MULTIPLEXER_BUILD"

    # Check if we need to compile
    NEED_COMPILE=0
    BINARY_PATH="$MULTIPLEXER_BUILD/ts-multiplexer"

    # Check if binary exists
    if [ ! -f "$BINARY_PATH" ]; then
        echo "[multiplexer] Binary not found - compiling..."
        NEED_COMPILE=1
    else
        # Check if any source file is newer than the binary
        if [ -n "$(find /app/src -name '*.cpp' -newer "$BINARY_PATH" 2>/dev/null)" ] || \
           [ -n "$(find /app/src -name '*.h' -newer "$BINARY_PATH" 2>/dev/null)" ] || \
           [ /app/CMakeLists.txt -nt "$BINARY_PATH" ]; then
            echo "[multiplexer] Source files changed - recompiling..."
            NEED_COMPILE=1
        else
            echo "[multiplexer] Binary is up to date - skipping compilation"
        fi
    fi

    # Compile if needed
    if [ "$NEED_COMPILE" -eq 1 ]; then
        echo "[multiplexer] Running CMake..."
        cmake -DTSDUCK_INSTALL_PREFIX="$TSDUCK_INSTALL" .. || { echo "CMake failed!"; exit 1; }
        
        echo "[multiplexer] Running Make..."
        make -j$(nproc) || { echo "Make failed!"; exit 1; }
        
        echo "[multiplexer] Compilation complete!"
    fi
    
    # Ensure binary is executable
    chmod +x "$BINARY_PATH"
}

# ============================================================================
# Main Execution
# ============================================================================

echo "=== Phase 1: TSDuck Setup ==="
ensure_tsduck

echo ""
echo "=== Phase 2: Multiplexer Compilation ==="
compile_multiplexer

echo ""
echo "=== Phase 3: Starting Multiplexer ==="
echo "Starting: $@"

# Remove trap since exec will replace this shell with the target process
trap - SIGTERM SIGINT

# Execute the binary from the build directory
if [ "$1" = "ts-multiplexer" ]; then
    shift
    exec "$MULTIPLEXER_BUILD/ts-multiplexer" "$@"
else
    exec "$@"
fi