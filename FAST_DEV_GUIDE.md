# Fast Development Workflow Guide

## Overview

This project uses a **host-mounted build system** where ALL build artifacts are stored on your host filesystem. The Docker image contains only build dependencies - no compiled binaries. This means:

1. **`docker compose build multiplexer`** → **seconds** (no compilation happens at build time)
2. **First container start** → ~35 min (TSDuck compiles once to host)
3. **Subsequent starts** → seconds (uses cached TSDuck from host)
4. **`docker system prune -a`** → no impact (everything on host filesystem)
5. **Code changes** → 5-10 seconds (only multiplexer recompiles)

## Build Time Comparison

| Operation | Old Multi-Stage | New Host-Mounted |
|-----------|-----------------|------------------|
| `docker compose build` | ~35 min | **~10 seconds** |
| First container start | ~30 sec | ~35 min* |
| Code change + restart | 5-10 sec | 5-10 sec |
| After `docker system prune` | ~35 min | **~10 seconds** |
| After `docker builder prune` | ~35 min | **~10 seconds** |

*First start only - TSDuck compiles once and is cached on host forever

## Quick Start

### Initial Setup (One Time Only)

```bash
# 1. Build the container image (fast - just installs dependencies)
docker compose build multiplexer

# 2. Start the container (first time will compile TSDuck ~35 min)
docker compose up multiplexer
```

The first startup will:
1. Clone TSDuck source to `./shared/tsduck-src/`
2. Compile TSDuck to `./shared/tsduck-build/`
3. Install TSDuck binaries to `./shared/tsduck/`
4. Compile the multiplexer to `./shared/multiplexer-build/`

All subsequent starts will skip steps 1-3 entirely.

### Daily Development Workflow

```bash
# 1. Edit your code in ./src/ on your host machine
vim src/Multiplexer.cpp

# 2. Restart the container (auto-recompiles if needed)
docker compose restart multiplexer

# That's it! Takes 5-10 seconds.
```

## How It Works

### Host-Mounted Build System

All build artifacts are stored on your host filesystem in `./shared/`:

```
./shared/
├── tsduck-src/              # TSDuck source code (cloned from GitHub)
│   ├── src/
│   ├── Makefile
│   └── .clone_timestamp
├── tsduck-build/            # TSDuck build artifacts (from make)
│   └── .build_timestamp
├── tsduck/                  # Installed TSDuck binaries
│   ├── bin/                 # ts* executables
│   ├── lib/                 # libtsduck.so, libtscore.so
│   ├── include/             # TSDuck headers
│   └── .install_timestamp
├── multiplexer-build/       # Multiplexer build artifacts
│   ├── CMakeCache.txt
│   ├── *.o                  # Object files
│   └── ts-multiplexer       # Compiled binary
└── ssl/                     # nginx-proxy SSL certificates
```

### Docker Image Contents

The Docker image contains **only dependencies**, no compiled code:

- Ubuntu 22.04 base
- Build tools: cmake, gcc, git, make
- TSDuck dependencies: libssl, libcurl, libedit, libpcap, libdvbcsa
- Multiplexer dependencies: yaml-cpp, zlib
- Runtime: ffmpeg, diagnostic tools
- Entrypoint script

### Volume Mounts

```yaml
volumes:
  # Source code (read-only)
  - ./src:/app/src:ro
  - ./CMakeLists.txt:/app/CMakeLists.txt:ro
  - ./config.yaml:/app/config.yaml:ro
  
  # TSDuck build system (read-write)
  - ./shared/tsduck-src:/opt/tsduck-src
  - ./shared/tsduck-build:/opt/tsduck-build
  - ./shared/tsduck:/opt/tsduck
  
  # Multiplexer build (read-write)
  - ./shared/multiplexer-build:/app/build
```

### Startup Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                    Container Startup                             │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │ TSDuck installed│
                    │  on host mount? │
                    └────────┬────────┘
                             │
              ┌──────────────┴──────────────┐
              │ No                          │ Yes
              ▼                             ▼
    ┌─────────────────┐           ┌─────────────────┐
    │ Clone TSDuck    │           │ Setup env vars  │
    │ source to host  │           │ PATH, LD_PATH   │
    └────────┬────────┘           └────────┬────────┘
             │                             │
             ▼                             │
    ┌─────────────────┐                    │
    │ Build TSDuck    │                    │
    │ ~35 min         │                    │
    └────────┬────────┘                    │
             │                             │
             ▼                             │
    ┌─────────────────┐                    │
    │ Install TSDuck  │                    │
    │ to host mount   │                    │
    └────────┬────────┘                    │
             │                             │
             └──────────────┬──────────────┘
                            │
                            ▼
                  ┌─────────────────┐
                  │ Source changed? │
                  └────────┬────────┘
                           │
            ┌──────────────┴──────────────┐
            │ Yes                         │ No
            ▼                             ▼
   ┌─────────────────┐          ┌─────────────────┐
   │ Compile         │          │ Skip compile    │
   │ multiplexer     │          │                 │
   └────────┬────────┘          └────────┬────────┘
            │                            │
            └──────────────┬─────────────┘
                           │
                           ▼
                  ┌─────────────────┐
                  │ Run             │
                  │ ts-multiplexer  │
                  └─────────────────┘
```

## Common Tasks

### View Container Logs

```bash
docker compose logs -f multiplexer
```

### Force Rebuild TSDuck

If you need to rebuild TSDuck (e.g., new version):

```bash
# Remove TSDuck installation (keeps source)
rm -rf ./shared/tsduck/*

# Or remove everything including source
rm -rf ./shared/tsduck-src/*
rm -rf ./shared/tsduck-build/*
rm -rf ./shared/tsduck/*

# Restart - will rebuild
docker compose restart multiplexer
```

### Force Rebuild Multiplexer

```bash
# Remove multiplexer build cache
rm -rf ./shared/multiplexer-build/*

# Restart - will recompile
docker compose restart multiplexer
```

### Rebuild Docker Image (Fast!)

```bash
# This is now fast because image has no compiled code
docker compose build multiplexer

# Even with --no-cache, still fast
docker compose build multiplexer --no-cache
```

### Manual Compilation

```bash
# Enter the container
docker compose exec multiplexer bash

# Manually compile
cd /app/build
cmake ..
make -j$(nproc)

# Exit and restart
exit
docker compose restart multiplexer
```

### Regenerate SSL Certificate

```bash
# Remove existing certificates
rm -rf ./shared/ssl/*

# Restart nginx-proxy to generate new certificate
docker compose restart nginx-proxy
```

## Troubleshooting

### "pkg-config: tsduck not found"

TSDuck isn't installed. Either:
1. Wait for first startup to complete (~35 min)
2. Or check `./shared/tsduck/lib/pkgconfig/tsduck.pc` exists

### "Binary not found" on startup

Normal on first startup or after clearing build cache. The entrypoint will compile it.

### "Source files changed - recompiling" but nothing changed

CMake tracks timestamps. Touch a file to force recompilation:

```bash
touch src/main.cpp
docker compose restart multiplexer
```

### TSDuck build fails

Check the logs for specific errors:

```bash
docker compose logs multiplexer
```

Common issues:
- Network problems cloning from GitHub
- Missing build dependencies (shouldn't happen with the Dockerfile)
- Disk space issues

### Very slow builds even after caching

Verify host mounts have content:

```bash
ls -la ./shared/tsduck/bin/
ls -la ./shared/tsduck/lib/
```

If empty, the volume might not be mounted correctly. Check docker-compose.yml.

## Architecture Details

### Why Host Mounts Instead of Docker Volumes?

1. **Survives `docker system prune -a`** - Docker volumes get deleted, host dirs don't
2. **Survives `docker builder prune`** - Build cache gets deleted, host dirs don't
3. **Easy inspection** - Just `ls ./shared/tsduck/` to see installed files
4. **Easy cleanup** - Just `rm -rf ./shared/tsduck-*` to reset
5. **Portable** - Copy `./shared/tsduck/` to another machine to share builds

### Why Compile at Runtime Instead of Build Time?

1. **Fast image builds** - `docker compose build` takes seconds
2. **Flexible** - Same image works with different TSDuck versions
3. **Debuggable** - Can see compile errors in logs, not buried in build output
4. **Recoverable** - If build fails, fix and restart (no need to rebuild image)

## Comparison to Old Approach

### Old Multi-Stage Dockerfile
- TSDuck compiled FROM SOURCE during `docker compose build`
- TSDuck binaries baked INTO the Docker image (~500MB+)
- Any `--no-cache` or builder prune = 35 min rebuild
- `docker system prune` = 35 min rebuild

### New Host-Mounted System
- `docker compose build` = seconds (only installs deps)
- TSDuck compiled at container start (first time only)
- TSDuck stored on HOST filesystem
- `docker system prune -a && docker compose build` = seconds
- TSDuck compiled once, never again (unless you delete `./shared/tsduck/`)

## Summary

**Never wait 35 minutes for `docker compose build` again!**

- Image build: **~10 seconds** (always)
- First container start: ~35 minutes (one time)
- Subsequent starts: **seconds** (TSDuck cached on host)
- After `docker system prune -a`: **~10 seconds** to rebuild image

Your TSDuck binaries live in `./shared/tsduck/` on your host filesystem. This cache survives Docker image rebuilds, `docker system prune`, and even Docker itself being reinstalled.