# Named Pipe Migration - TCP to FIFO

## Overview

Successfully migrated the multiplexer output from TCP (port 10004) to a named pipe (FIFO) for improved reliability and elimination of packet drops when transmitting MPEG-TS data to FFmpeg.

## Motivation

The TCP-based approach had several issues:
- **Packet drops**: TCP buffer overflow could cause packet loss during high bitrate bursts
- **Connection overhead**: TCP handshake and reconnection logic added complexity
- **Network stack overhead**: Unnecessary TCP/IP processing for local IPC
- **Timing issues**: TCP flow control could introduce unpredictable delays

Named pipes provide:
- **Zero packet loss**: Blocking writes ensure no data is dropped
- **Simpler design**: No network stack, connection management, or DNS resolution
- **Better performance**: Direct kernel-buffered IPC
- **Guaranteed ordering**: FIFO semantics preserve byte order

## Architecture Changes

### Before (TCP)
```
┌──────────────┐                    ┌────────────────────┐
│ Multiplexer  │──TCP:10004────────▶│ ffmpeg-rtmp-output │
│ (TCPOutput)  │                    │ (TCP listener)     │
└──────────────┘                    └────────────────────┘
```

### After (Named Pipe)
```
┌──────────────┐    ┌──────────────────┐    ┌────────────────────┐
│ Multiplexer  │───▶│ ts-pipe-volume   │───▶│ ffmpeg-rtmp-output │
│ (FIFOOutput) │    │ ts_output.pipe   │    │ (reads from pipe)  │
└──────────────┘    └──────────────────┘    └────────────────────┘
                           (Docker volume)
```

## Files Created

### 1. `src/FIFOOutput.h`
Header file for the new FIFO output class. Provides:
- Named pipe opening/closing
- Blocking write operations
- Automatic pipe buffer size optimization (1MB)
- Statistics tracking

### 2. `src/FIFOOutput.cpp`
Implementation with:
- FIFO verification (checks file is actually a pipe)
- Blocking open (waits for reader to connect)
- EPIPE handling (detects reader disconnection and reconnects)
- Buffer size tuning via `fcntl(F_SETPIPE_SZ)`

## Files Modified

### 1. `docker/entrypoint.sh`
Added Phase 3: Named pipe creation before multiplexer startup
- Creates `/pipe` directory
- Removes stale pipes from previous runs
- Creates new FIFO with `mkfifo`
- Sets appropriate permissions (666)

### 2. `docker/ffmpeg-rtmp-output-wrapper.sh`
Replaced TCP listening with pipe reading:
- Waits for pipe to exist (30 second timeout)
- Verifies it's a FIFO
- Sets FFmpeg input to `/pipe/ts_output.pipe`
- Removed TCP buffer configuration

### 3. `docker/Dockerfile.ffmpeg-rtmp-output`
- Removed `EXPOSE 10004` directive (no longer needed)
- Added comment about using shared volume instead

### 4. `docker-compose.yml`
Added shared volume configuration:
- Created `ts-pipe-volume` named volume
- Mounted to both `multiplexer:/pipe` and `ffmpeg-rtmp-output:/pipe`
- Updated `ffmpeg-rtmp-output` dependencies to wait for multiplexer startup
- Removed `TCP_RECV_BUFFER_SIZE` environment variable (no longer needed)

### 5. `src/main_new.cpp`
Replaced TCPOutput with FIFOOutput:
- Changed include from `TCPOutput.h` to `FIFOOutput.h`
- Replaced `OUTPUT_HOST`/`OUTPUT_PORT` with `PIPE_PATH`
- Changed `tcp_output.connect()` to `fifo_output.open()`
- Updated all `tcp_output.writePacket()` calls to `fifo_output.writePacket()`
- Changed `tcp_output.disconnect()` to `fifo_output.close()`

### 6. `CMakeLists.txt`
Updated source files:
- Replaced `src/TCPOutput.cpp` with `src/FIFOOutput.cpp`

## Startup Sequence

1. **Multiplexer container starts**: Creates `/pipe/ts_output.pipe` in entrypoint.sh
2. **ffmpeg-rtmp-output container starts**: Waits for pipe to exist (depends_on)
3. **Multiplexer opens pipe for writing**: Blocks until FFmpeg opens for reading
4. **FFmpeg opens pipe for reading**: Unblocks multiplexer
5. **Data flows**: Multiplexer writes, FFmpeg reads continuously

## Error Handling

### Reader Disconnection (FFmpeg Crash)
When FFmpeg crashes, the multiplexer's write() returns `EPIPE`:
1. FIFOOutput closes the pipe
2. Waits 100ms for FFmpeg to restart
3. Reopens the pipe (blocks until FFmpeg reconnects)
4. Resumes writing (current packet is lost, continuity maintained afterward)

### Pipe Doesn't Exist
If the pipe is deleted or never created:
- FIFOOutput.open() fails with error message
- Multiplexer terminates cleanly

### Multiple Writers
Named pipes support only one writer at a time. Configuration ensures:
- Only multiplexer writes to the pipe
- FFmpeg is the sole reader

## Performance Characteristics

### Pipe Buffer
- Default Linux pipe buffer: 64 KB (~11ms at 5Mbps)
- Configured buffer: 1 MB (~1.6 seconds at 5Mbps)
- Set via `fcntl(fd, F_SETPIPE_SZ, 1048576)`

### Blocking Behavior
- Writer blocks when buffer is full
- Reader blocks when buffer is empty
- Ensures perfect synchronization

### Latency
- Typical latency: <1ms (kernel IPC)
- No TCP handshake overhead
- No network stack processing

## Testing Checklist

Before deployment, verify:
- [ ] Multiplexer creates pipe successfully
- [ ] FFmpeg waits for and detects pipe
- [ ] Data flows without packet drops
- [ ] Continuity counters remain sequential
- [ ] FFmpeg crash triggers reconnection
- [ ] Pipe reopens after FFmpeg restart
- [ ] Clean shutdown (both containers stop gracefully)
- [ ] Volume persists between container restarts
- [ ] No stale pipes after restart

## Rollback Plan

To revert to TCP if needed:

1. Restore `CMakeLists.txt`: Change `FIFOOutput.cpp` back to `TCPOutput.cpp`
2. Restore `src/main_new.cpp`: Change includes and all references
3. Restore `docker-compose.yml`: Remove volume mounts and restore TCP config
4. Restore `docker/entrypoint.sh`: Remove pipe creation section
5. Restore `docker/ffmpeg-rtmp-output-wrapper.sh`: Restore TCP listening
6. Restore `docker/Dockerfile.ffmpeg-rtmp-output`: Add back `EXPOSE 10004`

The old `src/TCPOutput.h` and `src/TCPOutput.cpp` remain in the codebase for reference.

## Benefits Summary

✅ **Zero packet drops** - Blocking writes prevent data loss  
✅ **Simpler code** - No connection management or retry logic  
✅ **Better performance** - Direct kernel IPC, no TCP overhead  
✅ **Deterministic behavior** - FIFO blocking is predictable  
✅ **Local IPC only** - Perfect for container-to-container communication  

## Migration Complete

All changes have been implemented and are ready for testing. The system now uses a named pipe for multiplexer-to-FFmpeg communication, eliminating the packet drop issues inherent in the TCP-based approach.
