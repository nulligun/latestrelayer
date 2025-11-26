# TSDuck Build Optimization

## Current Approach (Fast Development)

As of November 2024, this project uses an **optimized multi-stage Docker build** that eliminates the 30-minute rebuild problem.

### Key Features

1. **TSDuck Pre-built & Cached**: Compiled once in a builder stage, then Docker caches it forever
2. **Source Code Mounted**: Your multiplexer code (`./src/`) is mounted from the host for instant changes
3. **Auto-Compilation**: Container automatically recompiles only your code when source files change
4. **Incremental Builds**: Persistent build volume caches CMake artifacts and object files

### Build Times

- **Initial build**: ~35 minutes (TSDuck compiles once, then cached)
- **Subsequent builds**: ~2 minutes (TSDuck reused from cache)
- **Code changes**: 5-10 seconds (incremental compilation)

### Quick Start

```bash
# One-time initial build
docker compose build multiplexer  # ~35 min first time

# Daily development
vim src/Multiplexer.cpp           # Edit your code
docker compose restart multiplexer # ~5-10 sec auto-recompile
```

See **[FAST_DEV_GUIDE.md](FAST_DEV_GUIDE.md)** for complete documentation.

---

## Historical Context (Old Approach)

The information below documents the **previous approach** which is no longer used but kept for reference.

### Previous Build Issues (DEPRECATED)

Fixed compilation errors when building the TSDuck multiplexer with TSDuck v3.43. The build was failing due to multiple issues:

1. **C++17 `[[maybe_unused]]` Attribute Warning** - GCC treating ignored attribute as error with `-Werror`
2. **Vatek SDK Dependency Missing** - TSDuck trying to compile hardware support requiring external SDK
3. **Documentation Build Failure** - Missing asciidoc tools
4. **C++ Standard Mismatch** - TSDuck requires C++20
5. **TSDuck API Changes** - Constructor and method incompatibilities

### Old Fixes Applied (DEPRECATED)

#### Dockerfile Changes
```dockerfile
# Old approach - compiled TSDuck from source every time
RUN git clone https://github.com/tsduck/tsduck.git && \
    cd tsduck && \
    make NOTELETEXT=1 NOSRT=1 NORIST=1 NODTAPI=1 NOVATEK=1 NOWARNING=1 CXXFLAGS_EXTRA="-Wno-error=attributes" && \
    make install NOTELETEXT=1 NOSRT=1 NORIST=1 NODTAPI=1 NOVATEK=1 NOWARNING=1 NODOC=1
```

**Problem**: This ran on every build, taking 25-30 minutes every time.

#### CMakeLists.txt Changes
```cmake
# Changed to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```

#### TSAnalyzer Code Updates

Added `DuckContext` member and updated PAT/PMT parsing to match TSDuck 3.43 API:

```cpp
// Constructor
TSAnalyzer::TSAnalyzer() : duck_(), demux_(duck_) { }

// PAT/PMT parsing with new API
auto section = std::make_shared<ts::Section>(payload, payload_size, ts::PID_PAT, ts::CRC32::CHECK);
ts::BinaryTable bin_table;
bin_table.addSection(section);
ts::PAT pat;
if (pat.deserialize(duck_, bin_table) && pat.isValid()) { ... }
```

**These code changes are still valid** - the C++20 requirement and TSDuck API updates remain in the current codebase.

---

## Migration from Old to New Approach

### What Changed

**Old Workflow**:
- TSDuck compiled from source in Dockerfile
- Multiplexer code baked into Docker image  
- Every change required full rebuild (~30 min)
- No caching, no incremental builds

**New Workflow**:
- Multi-stage build: TSDuck in stage 1 (cached), runtime in stage 2
- Source code mounted from host (`./src:/app/src:ro`)
- Persistent build volume for CMake cache
- Auto-compile entrypoint script
- Result: 99% faster iteration (30 min → 5-10 sec)

### Files Modified in Migration

- **[`docker/Dockerfile.multiplexer`](docker/Dockerfile.multiplexer)** - Multi-stage build with cached TSDuck
- **[`docker-compose.yml`](docker-compose.yml)** - Added source mounts and build volume
- **[`docker/entrypoint.sh`](docker/entrypoint.sh)** - Created auto-compile script
- **[`FAST_DEV_GUIDE.md`](FAST_DEV_GUIDE.md)** - New comprehensive development guide

### Code Compatibility

The multiplexer source code remains unchanged - the C++20 standard and TSDuck 3.43 API compatibility fixes documented above are still in use.

---

## Technical Details

### TSDuck Version

Currently using: **TSDuck 3.43** (latest from GitHub)

Compiled with flags:
- `NOTELETEXT=1` - Disable teletext support
- `NOSRT=1` - Disable SRT protocol
- `NORIST=1` - Disable RIST protocol
- `NODTAPI=1` - Disable DTAPI support
- `NOVATEK=1` - Disable Vatek hardware support
- `NODVBNIP=1` - Disable DVB-NIP (DVB Network IP) support
- `NOWARNING=1` - Treat warnings as warnings not errors
- `NODOC=1` - Skip documentation build
- `CXXFLAGS_EXTRA="-Wno-error=attributes"` - Suppress attribute warnings

### Build Architecture

```
┌────────────────────────────────┐
│ Stage 1: tsduck-builder        │
│ • FROM ubuntu:22.04            │
│ • Install build dependencies   │
│ • git clone TSDuck             │
│ • make && make install         │
│ • Result: /usr/bin/ts*         │
│          /usr/lib/libtsduck.so │
└───────────┬────────────────────┘
            │ COPY (cached layer)
            ▼
┌────────────────────────────────┐
│ Stage 2: Runtime               │
│ • FROM ubuntu:22.04            │
│ • Install runtime deps         │
│ • COPY TSDuck from Stage 1     │
│ • Install build tools          │
│ • COPY entrypoint script       │
└────────────────────────────────┘
```

### Volume Structure

```yaml
volumes:
  - ./src:/app/src:ro                    # Source code
  - ./CMakeLists.txt:/app/CMakeLists.txt:ro  # Build config
  - multiplexer-build:/app/build         # Persistent cache
```

## Troubleshooting

### TSDuck Compilation Errors

If TSDuck fails to compile during the initial build:

1. Check Docker has enough resources (8GB+ RAM recommended)
2. Review build logs for specific errors
3. Consider using a pre-built TSDuck if compilation continues to fail

#### DVB-NIP Module Compilation Error (Fixed)

**Issue**: As of late 2024, the DVB-NIP (DVB Network IP) module in TSDuck fails to compile with this error:

```
dtv/dvbnip/tsMulticastGatewayConfigurationTransportSession.cpp: error:
no match for 'operator<<' (operand types are 'std::ostream' and 'const milliseconds')
```

**Root Cause**: The DVB-NIP module attempts to stream `std::chrono::milliseconds` objects, but TSDuck's custom stream operators don't provide an overload for chrono duration types.

**Solution**: Disable the DVB-NIP module by adding `NODVBNIP=1` to the build flags. This module provides professional DVB-over-IP broadcasting features (multicast gateway, FLUTE protocol) which are not needed for basic MPEG-TS multiplexing.

**Impact**: None. The multiplexer only uses TSDuck's core transport stream processing capabilities (packet parsing, PMT/PAT tables, PID extraction, timestamp management). DVB-NIP is only needed for carrier-grade DVB broadcasting infrastructure.

### Multiplexer Compilation Errors  

If your code fails to compile:

1. Check logs: `docker compose logs multiplexer`
2. Verify C++20 compatibility
3. Ensure TSDuck API usage matches v3.43
4. Manual compile for debugging:
   ```bash
   docker compose exec multiplexer bash
   cd /app/build && cmake .. && make
   ```

### Performance Issues

If builds are still slow:

1. Ensure Docker cache is working: `docker system df`
2. Verify source is mounted (not copied): `docker compose config`
3. Check build volume exists: `docker volume ls | grep multiplexer`

## References

- TSDuck Repository: https://github.com/tsduck/tsduck
- TSDuck Documentation: https://tsduck.io/
- Original Build Fix: This document (historical section above)
- New Development Guide: [FAST_DEV_GUIDE.md](FAST_DEV_GUIDE.md)

## Summary

**Current Status**: ✅ Optimized multi-stage build with caching  
**Build Time**: 5-10 seconds for code changes (after initial ~35 min setup)  
**TSDuck Version**: 3.43  
**C++ Standard**: C++20  

For development workflow, see **[FAST_DEV_GUIDE.md](FAST_DEV_GUIDE.md)**.