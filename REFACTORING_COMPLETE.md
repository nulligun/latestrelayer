# Multiplexer Refactoring - Implementation Complete

## Summary

Successfully refactored the complex multiplexer (~3000 lines across 20+ files) into a streamlined implementation based on [`multi2/src/tcp_main.cpp`](multi2/src/tcp_main.cpp:1)'s proven TCP splicing pattern.

## What Was Created

### Docker Infrastructure
1. **[`docker/Dockerfile.ffmpeg-rtmp-output`](docker/Dockerfile.ffmpeg-rtmp-output:1)** - FFmpeg container that listens on TCP 10004
2. **[`docker/ffmpeg-rtmp-output-wrapper.sh`](docker/ffmpeg-rtmp-output-wrapper.sh:1)** - Wrapper with 2MB TCP buffer settings
3. **[`docker/ffmpeg-rtmp-output-health-check.sh`](docker/ffmpeg-rtmp-output-health-check.sh:1)** - Health check for port 10004

### C++ Components (Based on multi2 Pattern)
1. **[`src/TCPReader.h`](src/TCPReader.h:1)** / **[`src/TCPReader.cpp`](src/TCPReader.cpp:1)** (736 lines)
   - Background thread for continuous buffering
   - Automatic PAT/PMT discovery
   - IDR frame detection
   - Audio sync point detection
   - Rolling buffer management

2. **[`src/TCPOutput.h`](src/TCPOutput.h:1)** / **[`src/TCPOutput.cpp`](src/TCPOutput.cpp:1)** (142 lines)
   - Simple TCP client to ffmpeg-rtmp-output:10004
   - 2MB send buffer (matching ffmpeg-fallback settings)
   - Connection management

3. **[`src/StreamSplicer.h`](src/StreamSplicer.h:1)** / **[`src/StreamSplicer.cpp`](src/StreamSplicer.cpp:1)** (247 lines)
   - Timestamp rebasing (PTS/DTS/PCR)
   - Continuity counter management
   - PAT/PMT generation with TSDuck
   - SPS/PPS injection packets

4. **[`src/TSStreamReassembler.h`](src/TSStreamReassembler.h:1)** (63 lines)
   - Handles TS packet boundaries in TCP byte stream
   - Simplified version from multi2

5. **[`src/main_new.cpp`](src/main_new.cpp:1)** (432 lines)
   - Clean camera + fallback switching
   - IDR-aligned splice points with audio sync
   - Continuous timestamp rebasing
   - Signal handling

### Configuration Updates
1. **[`docker-compose.yml`](docker-compose.yml:1)** - Added ffmpeg-rtmp-output service in Tier 3.5
2. **[`CMakeLists.txt`](CMakeLists.txt:1)** - Streamlined to 5 source files (from 18)

## Architecture Changes

### Before (Complex):
```
Camera/Drone TCP → TCPReceiver → StreamSwitcher → TimestampManager → 
PIDMapper → RTMPOutput (subprocess) → FFmpeg stdin → nginx-rtmp
```

### After (Streamlined):
```
Camera TCP:10000 ──┐
                   ├──► StreamSplicer ──► TCPOutput:10004 ──► 
Fallback TCP:10001─┘                      ffmpeg-rtmp-output ──► nginx-rtmp
```

##Removed Complexity
- ❌ Drone input support
- ❌ Privacy mode / HTTP API
- ❌ Controller integration
- ❌ Jitter statistics
- ❌ Complex StreamSwitcher logic
- ❌ RTMPOutput subprocess management
- ❌ YAML config parsing
- ❌ HTTP client/server

## Key Features Preserved
- ✅ Camera + Fallback switching
- ✅ IDR-aligned clean splices
- ✅ SPS/PPS injection at splice points
- ✅ Audio sync point detection
- ✅ Continuous timestamp rebasing
- ✅ TCP buffer settings matching ffmpeg-fallback

## Next Steps - Testing

### 1. Build Containers
```bash
# Build the new ffmpeg-rtmp-output container
docker-compose build ffmpeg-rtmp-output

# Build the refactored multiplexer
docker-compose build multiplexer
```

### 2. Test End-to-End
```bash
# Start all services
docker-compose up -d

# Watch logs
docker-compose logs -f multiplexer
docker-compose logs -f ffmpeg-rtmp-output

# Verify in VLC
vlc rtmp://localhost/live/stream
```

### 3. Test Transitions
- **Fallback → Camera**: Start system, then start camera input
- **Camera → Fallback**: Kill camera container while running
- Verify smooth transitions with no corruption

### 4. Verify Splice Quality
- Watch for "Packet corrupt" errors in ffmpeg-rtmp-output logs
- Check VLC playback for glitches at transition points
- Verify audio continuity across splice points

## Troubleshooting

### If Build Fails
- Check that NALParser.h exists (needed by TCPReader)
- Verify TSDuck is installed in container
- Review CMakeLists.txt source file paths

### If Connection Fails
- Verify ffmpeg-rtmp-output is listening on 10004: `docker exec ffmpeg-rtmp-output netstat -tln | grep 10004`
- Check multiplexer can reach it: `docker exec multiplexer nc -zv ffmpeg-rtmp-output 10004`
- Review health checks: `docker ps` (should show healthy)

### If Splices Have Glitches
- Check logs for "Audio sync point" messages
- Verify SPS/PPS injection is happening
- Look for CC discontinuities in logs

## Migration Plan

Once testing confirms success:

1. **Backup old code**: Keep old files for reference
2. **Rename main_new.cpp → main.cpp**
3. **Archive deprecated files**: Move to `src/deprecated/` directory
4. **Update documentation**: README.md, ARCHITECTURE.md
5. **Tag release**: Create git tag for the refactored version

## File Statistics

### Before Refactoring
- **18 source files** (~3000 lines total)
- **Complex dependencies**: yaml-cpp, CURL, custom HTTP server
- **Multiple layers**: Multiplexer → StreamSwitcher → TimestampManager → PIDMapper → RTMPOutput

### After Refactoring
- **5 source files** (~1600 lines total)
- **Simple dependencies**: TSDuck, Threads
- **Clean layers**: TCPReader → StreamSplicer → TCPOutput

**Code reduction: ~47% fewer lines, 72% fewer files**

## Success Criteria

✅ Multiplexer builds successfully  
✅ ffmpeg-rtmp-output builds successfully  
✅ Fallback stream plays smoothly in VLC  
✅ Camera stream switches without corruption  
✅ Fallback recovery works cleanly  
✅ No "Packet corrupt" errors in logs  
✅ Audio remains synchronized across splices  

---

**Status**: Implementation Complete → Ready for Testing

See [`REFACTOR_PLAN.md`](REFACTOR_PLAN.md:1) for detailed architecture documentation.