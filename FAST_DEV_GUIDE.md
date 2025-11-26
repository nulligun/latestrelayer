# Fast Development Workflow Guide

## Overview

This project now uses an optimized build system that eliminates the 30-minute rebuild problem. The key improvements are:

1. **TSDuck Pre-compiled & Host-Cached**: Built once and persisted to host filesystem (survives even `docker system prune`)
2. **Source Code Mounted**: Your code changes are instantly visible in the container
3. **Auto-compilation**: Container automatically recompiles only when source changes
4. **Persistent Build Cache**: CMake cache and object files persist on host filesystem
5. **SSL Certificate Persistence**: Self-signed certs persist across container rebuilds

## Build Time Comparison

| Operation | Old | New | Improvement |
|-----------|-----|-----|-------------|
| Initial build | 30 min | ~35 min* | First time only |
| Rebuild container | 30 min | ~2 min | 93% faster |
| Code change iteration | 30 min | 5-10 sec | 99% faster |
| After `docker system prune` | 30 min | ~2 min** | 93% faster |

*First build compiles TSDuck once, then it's cached to host filesystem
**TSDuck binaries are restored from host cache, only multiplexer recompiles

## Quick Start

### Initial Setup (One Time Only)

```bash
# Build the container (TSDuck will compile and be cached)
docker compose build multiplexer

# This takes ~35 minutes the first time
# After this, TSDuck never needs to compile again!
```

### Daily Development Workflow

```bash
# 1. Edit your code in ./src/ on your host machine
vim src/Multiplexer.cpp

# 2. Restart the container (auto-recompiles if needed)
docker compose restart multiplexer

# That's it! Takes 5-10 seconds.
```

### How It Works

#### Source Code Mounting

Your source files are mounted from the host:
- `./src` → `/app/src` (read-only)
- `./CMakeLists.txt` → `/app/CMakeLists.txt` (read-only)

Changes you make on your host are immediately visible in the container.

#### Auto-Compilation on Startup

The container's entrypoint script ([`docker/entrypoint.sh`](docker/entrypoint.sh)):
1. Checks if the binary exists
2. Compares timestamps of source files vs binary
3. Only recompiles if source is newer than binary
4. Uses cached build artifacts for incremental compilation

#### Host-Mounted Shared Directory

All persistent data is stored in `./shared/` on your host machine:

```
./shared/
├── ssl/                    # nginx-proxy SSL certificates
│   ├── cert.pem
│   └── key.pem
├── tsduck/                 # Pre-built TSDuck binaries
│   ├── bin/                # ts* executables
│   ├── lib/                # libtsduck.so, libtscore.so
│   ├── include/            # TSDuck headers
│   └── share/              # pkgconfig files
└── multiplexer-build/      # CMake cache and build artifacts
    ├── CMakeCache.txt
    ├── *.o                 # Object files
    └── ts-multiplexer      # Compiled binary
```

**Benefits:**
- Survives `docker system prune`, image rebuilds, and container destruction
- SSL certificates persist (no more clicking "Accept" in browser)
- TSDuck binaries cached on host (skips 35-min rebuild)
- Incremental multiplexer compilation via cached object files

## Common Tasks

### View Container Logs

```bash
docker compose logs -f multiplexer
```

### Force Full Rebuild

If you need to recompile TSDuck (rare):

```bash
# Clear the host TSDuck cache
rm -rf ./shared/tsduck/*

# Delete Docker builder cache
docker builder prune -a

# Rebuild from scratch - TSDuck will be cached to host after build
docker compose build multiplexer --no-cache
```

### Clean Build Artifacts

```bash
# Remove the multiplexer build cache (on host)
rm -rf ./shared/multiplexer-build/*

# Next restart will do a full compile of multiplexer (not TSDuck)
docker compose restart multiplexer
```

### Regenerate SSL Certificate

```bash
# Remove existing certificates
rm -rf ./shared/ssl/*

# Restart nginx-proxy to generate new certificate
docker compose restart nginx-proxy
```

### Manual Compilation

If you prefer manual control:

```bash
# Enter the container
docker compose exec multiplexer bash

# Manually compile
cd /app/build
cmake ..
make -j$(nproc)
cp ts-multiplexer /usr/local/bin/

# Exit and restart
exit
docker compose restart multiplexer
```

## Architecture

### Multi-Stage Docker Build

```
┌─────────────────────────────────────┐
│   Stage 1: tsduck-builder          │
│   • Compiles TSDuck from source     │
│   • Only runs once, then cached     │
│   • Result: Pre-built binaries      │
└──────────────┬──────────────────────┘
               │ COPY binaries
               ▼
┌─────────────────────────────────────┐
│   Stage 2: Runtime                  │
│   • Lightweight base image          │
│   • TSDuck binaries from Stage 1    │
│   • Build tools (cmake, gcc)        │
│   • Auto-compile entrypoint         │
└─────────────────────────────────────┘
               ▲
               │ Volume mounts
┌──────────────┴──────────────────────┐
│   Host Files                        │
│   • ./src/*.cpp (your code)         │
│   • ./CMakeLists.txt                │
│   Build Cache Volume                │
│   • CMake cache                     │
│   • Object files                    │
└─────────────────────────────────────┘
```

### File Structure

**Host (./shared/):**
```
./shared/
├── ssl/                    # Mounted to nginx-proxy:/etc/nginx/ssl
├── tsduck/                 # Mounted to multiplexer:/opt/tsduck-cache
└── multiplexer-build/      # Mounted to multiplexer:/app/build
```

**Inside multiplexer container:**
```
/app/
├── src/               # Mounted from host (read-only)
├── CMakeLists.txt    # Mounted from host (read-only)
├── build/            # Host-mounted: ./shared/multiplexer-build
│   ├── CMakeCache.txt
│   ├── *.o           # Object files for incremental builds
│   └── ts-multiplexer
└── config.yaml       # Mounted from host (read-only)

/opt/tsduck-cache/     # Host-mounted: ./shared/tsduck
├── bin/               # TSDuck binaries (copied to /usr/bin on startup)
├── lib/               # TSDuck libraries (copied to /usr/lib on startup)
├── include/           # TSDuck headers
└── share/             # pkgconfig files

/usr/bin/
└── ts*               # TSDuck binaries (installed from cache or Docker layer)

/usr/lib/
├── libtsduck.so      # TSDuck library
└── tsduck/           # TSDuck plugins
```

## Troubleshooting

### "Binary not found" on startup

The first startup after building will compile the multiplexer. This is normal and takes ~30 seconds.

### "Source files changed - recompiling" but nothing changed

Check file timestamps. Touch a file to force recompilation:

```bash
touch src/main.cpp
docker compose restart multiplexer
```

### Compilation errors

View the full build output:

```bash
docker compose up multiplexer
```

### Very slow builds even after caching

Check Docker disk space:

```bash
docker system df
docker system prune  # Clean up if needed
```

## Comparison to Old Approach

### Old Workflow (TSDUCK_BUILD_FIX.md)
- Compiled TSDuck from source every build
- Source code baked into image
- Any change required full 30-minute rebuild
- No incremental compilation

### New Workflow
- TSDuck compiled once, cached to host filesystem
- Source code mounted from host
- Changes visible immediately
- Auto-recompile only if needed (~5-10 sec)
- Incremental builds via host-mounted cache
- Survives `docker system prune` and image rebuilds

## Advanced Tips

### Speed Up Initial TSDuck Build

The TSDuck compilation can be parallelized. If you have a powerful machine:

```dockerfile
# In docker/Dockerfile.multiplexer, line with make command:
make -j$(nproc) NOTELETEXT=1 ...
```

Already using all CPU cores with `$(nproc)`.

### Skip Auto-Compilation

If you want instant startup (no compilation check):

```bash
# Modify docker-compose.yml temporarily:
command: ["sh", "-c", "sleep infinity"]

# Then manually run when ready:
docker compose exec multiplexer /usr/local/bin/entrypoint.sh ts-multiplexer /app/config.yaml
```

### Development with IDE

Since source is mounted, you can use your local IDE:
- Edit code with VSCode, vim, etc. on your host
- Changes are immediately in container
- Restart container to apply

## Summary

**Never wait 30 minutes again!**

- Initial setup: ~35 minutes (one time)
- Container recreate: ~2 minutes
- Code changes: 5-10 seconds
- After `docker system prune`: ~2 minutes (TSDuck restored from host cache)

Your source code lives on your host, the container auto-compiles when needed, and TSDuck binaries are cached on your host filesystem in `./shared/tsduck/`. This cache survives Docker image rebuilds, container destruction, and even `docker system prune`.

SSL certificates are also cached in `./shared/ssl/`, so you won't need to click "Accept" in your browser after container restarts.