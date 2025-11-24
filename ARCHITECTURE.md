# TSDuck MPEG-TS Multiplexer Architecture

## Project Overview

A high-availability MPEG-TS multiplexer that consumes two UDP TS streams (live SRT input and looped fallback) and outputs a continuous RTMP stream with automatic failover and seamless timestamp management.

## Technology Stack

- **Language**: C++17
- **TS Library**: TSDuck (libtsduck)
- **Build System**: CMake 3.15+
- **Config Parser**: yaml-cpp
- **Container**: Docker (Ubuntu 22.04 base)
- **RTMP Output**: FFmpeg subprocess with stdin pipe

## Project Structure

```
tsduck-multiplexer/
├── CMakeLists.txt
├── config.yaml
├── docker-compose.yml
├── README.md
├── ARCHITECTURE.md
├── docker/
│   └── Dockerfile.multiplexer
├── src/
│   ├── main.cpp
│   ├── Config.h
│   ├── Config.cpp
│   ├── TSPacketQueue.h
│   ├── TSPacketQueue.cpp
│   ├── UDPReceiver.h
│   ├── UDPReceiver.cpp
│   ├── TSAnalyzer.h
│   ├── TSAnalyzer.cpp
│   ├── TimestampManager.h
│   ├── TimestampManager.cpp
│   ├── PIDMapper.h
│   ├── PIDMapper.cpp
│   ├── StreamSwitcher.h
│   ├── StreamSwitcher.cpp
│   ├── RTMPOutput.h
│   ├── RTMPOutput.cpp
│   └── Multiplexer.h
│   └── Multiplexer.cpp
└── fallback.mp4 (example fallback file)
```

## Core Components

### 1. Configuration Management (`Config.h/cpp`)

**Purpose**: Parse and validate YAML configuration file

**Key Fields**:
- `live_udp_port`: UDP port for live TS (e.g., 10000)
- `fallback_udp_port`: UDP port for fallback TS (e.g., 10001)
- `rtmp_url`: Full RTMP destination URL
- `max_live_gap_ms`: Timeout before switching to fallback (default: 2000ms)
- `log_level`: Logging verbosity

**Dependencies**: yaml-cpp

### 2. Thread-Safe Packet Queue (`TSPacketQueue.h/cpp`)

**Purpose**: Thread-safe FIFO queue for TS packets between UDP receivers and main processor

**Key Methods**:
```cpp
void push(const ts::TSPacket& packet);
bool pop(ts::TSPacket& packet, std::chrono::milliseconds timeout);
size_t size() const;
void clear();
```

**Implementation**:
- `std::queue<ts::TSPacket>` internal storage
- `std::mutex` for thread safety
- `std::condition_variable` for blocking pop operations
- Atomic size tracking

### 3. UDP Receiver (`UDPReceiver.h/cpp`)

**Purpose**: Dedicated thread that reads TS packets from UDP socket and pushes to queue

**Key Components**:
- Socket creation and binding (POSIX sockets)
- Infinite receive loop in dedicated thread
- 188-byte TS packet buffer
- Error handling for socket failures
- Graceful shutdown mechanism

**Flow**:
1. Create UDP socket on specified port
2. Start receiver thread
3. Loop: `recvfrom()` → validate 188 bytes → create `ts::TSPacket` → push to queue
4. On shutdown signal, close socket and join thread

### 4. TS Analyzer (`TSAnalyzer.h/cpp`)

**Purpose**: Extract and track PMT, PIDs, and elementary stream information

**TSDuck APIs Used**:
- `ts::TSPacket::getPID()`
- `ts::TSPacket::hasPCR()`
- `ts::TSPacket::getPCR()`
- `ts::TSPacket::hasPESStartCode()`
- Manual PMT parsing or use `ts::Table` classes

**Tracked Information**:
- PAT (Program Association Table)
- PMT (Program Map Table)
- Video PID and codec
- Audio PID and codec
- PCR PID

### 5. Timestamp Manager (`TimestampManager.h/cpp`)

**Purpose**: Maintain timeline continuity across source switches

**Key State**:
```cpp
struct SourceTimestamps {
    int64_t offset;           // Timestamp offset for this source
    int64_t last_original_pts;
    int64_t last_original_dts;
    int64_t last_original_pcr;
};

int64_t last_output_pts;
int64_t last_output_dts;
int64_t last_output_pcr;
```

**Core Logic**:
1. **Extract timestamps** from PES headers using TSDuck
2. **Apply source-specific offset**: `adjusted = original + source_offset`
3. **Enforce monotonicity**: If `adjusted < last_output`, bump forward
4. **Update state**: Store `last_output_*` values
5. **On source switch**: Calculate new offset to maintain continuity

**Timestamp Extraction**:
```cpp
// Using TSDuck PES parsing
ts::PES pes;
if (packet.getPESStart() && pes.initialize(packet)) {
    if (pes.hasPTS()) pts = pes.getPTS();
    if (pes.hasDTS()) dts = pes.getDTS();
}
```

**Timestamp Rewriting**:
- Modify PES header fields directly in `ts::TSPacket`
- Update PCR in adaptation field if present
- Use TSDuck's packet manipulation APIs

### 6. PID Mapper (`PIDMapper.h/cpp`)

**Purpose**: Remap fallback PIDs to match live stream PIDs

**Mapping Table**:
```cpp
std::map<uint16_t, uint16_t> fallback_to_live_pid;
// Example: fallback video PID 256 → live video PID 256
//          fallback audio PID 257 → live audio PID 257
```

**Continuity Counter Management**:
- Track last continuity counter per output PID
- When switching sources, maintain counter sequence
- Increment properly using `ts::TSPacket::setCC()`

### 7. Stream Switcher (`StreamSwitcher.h/cpp`)

**Purpose**: State machine for LIVE/FALLBACK mode selection

**States**:
```cpp
enum class Mode {
    LIVE,
    FALLBACK
};
```

**State Transitions**:
```
LIVE → FALLBACK: when (now - last_live_packet_time) > max_live_gap_ms
FALLBACK → LIVE: when consecutive live packets arrive within expected timing
```

**Gap Detection**:
- Store `last_live_packet_wallclock` (std::chrono::steady_clock)
- Check timeout in main loop
- Log transitions for observability

### 8. RTMP Output (`RTMPOutput.h/cpp`)

**Purpose**: Spawn FFmpeg process and pipe TS packets to RTMP

**FFmpeg Command**:
```bash
ffmpeg -re -i - \
  -c copy \
  -f flv "rtmp://server/live/key"
```

**Implementation**:
- Use `popen()` or `fork()`/`exec()` with pipe
- Write TS packets to FFmpeg stdin
- Monitor FFmpeg process health
- Restart on crash with exponential backoff

**Pacing**:
- Calculate inter-packet delay from PTS differences
- Use `std::this_thread::sleep_for()` to maintain real-time rate
- Fallback to fixed bitrate assumption if PTS unavailable

### 9. Main Multiplexer (`Multiplexer.h/cpp`)

**Purpose**: Orchestrate all components in main processing loop

**Initialization**:
1. Load configuration
2. Create packet queues
3. Start UDP receiver threads
4. Analyze first packets from each source to get PIDs
5. Initialize timestamp manager and PID mapper
6. Start FFmpeg RTMP output process

**Main Loop**:
```cpp
while (running) {
    Mode current_mode = switcher.getMode();
    
    // Update mode based on live packet timing
    switcher.checkLiveTimeout();
    
    // Select queue based on mode
    TSPacketQueue& queue = (current_mode == LIVE) ? live_queue : fallback_queue;
    
    ts::TSPacket packet;
    if (queue.pop(packet, 100ms)) {
        // 1. Extract original timestamps
        auto [pts, dts, pcr] = analyzer.extractTimestamps(packet);
        
        // 2. Apply timestamp offset and enforce monotonicity
        timestamp_mgr.adjustTimestamps(packet, current_mode);
        
        // 3. Remap PIDs if from fallback
        if (current_mode == FALLBACK) {
            pid_mapper.remapPacket(packet);
        }
        
        // 4. Fix continuity counters
        pid_mapper.fixContinuity(packet);
        
        // 5. Write to RTMP FFmpeg stdin
        rtmp_output.writePacket(packet);
        
        // 6. Update last packet time for live
        if (current_mode == LIVE) {
            switcher.updateLiveTime();
        }
    }
}
```

**Shutdown**:
1. Signal all threads to stop
2. Join UDP receiver threads
3. Close FFmpeg process
4. Clean up resources

## Threading Model

```
Main Thread:
  - Load config
  - Initialize components
  - Run main processing loop
  - Coordinate shutdown

UDP Live Thread:
  - Bind UDP socket (port 10000)
  - Receive loop → push to live_queue
  - Exit on shutdown signal

UDP Fallback Thread:
  - Bind UDP socket (port 10001)
  - Receive loop → push to fallback_queue
  - Exit on shutdown signal

FFmpeg Process:
  - Spawned subprocess
  - Reads TS from stdin pipe
  - Outputs RTMP stream
```

## Timestamp Continuity Algorithm

### Scenario 1: Normal Operation (LIVE mode)

```
Input (live):  PTS=1000, PTS=1040, PTS=1080...
Offset:        live_offset = 0
Output:        PTS=1000, PTS=1040, PTS=1080...
```

### Scenario 2: Switch to Fallback

```
Last live output: PTS=5000
Fallback input arrives: PTS=100, PTS=140...

Calculate fallback_offset:
  fallback_offset = last_output_pts - first_fallback_pts + frame_duration
  fallback_offset = 5000 - 100 + 40 = 4940

Output (fallback):
  adjusted_pts = 100 + 4940 = 5040
  adjusted_pts = 140 + 4940 = 5080
```

### Scenario 3: Return to Live

```
Last fallback output: PTS=6000
Live input resumes: PTS=3000, PTS=3040...

Calculate new live_offset:
  live_offset = last_output_pts - resumed_live_pts + frame_duration
  live_offset = 6000 - 3000 + 40 = 3040

Output (live resumed):
  adjusted_pts = 3000 + 3040 = 6040
  adjusted_pts = 3040 + 3040 = 6080
```

## Docker Architecture

### Network Topology

```
Docker Network: tsnet (bridge)

├─ multiplexer (service)
│  ├─ Listens UDP 10000 (from ffmpeg-srt-live)
│  ├─ Listens UDP 10001 (from ffmpeg-fallback)
│  └─ Outputs RTMP to external server
│
├─ ffmpeg-srt-live (service)
│  ├─ Exposed port: 1937/udp (SRT listener)
│  └─ Sends TS to multiplexer:10000
│
└─ ffmpeg-fallback (service)
   ├─ Loops fallback.mp4
   └─ Sends TS to multiplexer:10001
```

### Dockerfile Strategy

**Multiplexer Dockerfile**:
1. Base: `ubuntu:22.04`
2. Install build tools: cmake, g++, git
3. Clone and build TSDuck from source
4. Install yaml-cpp
5. Copy source code
6. Build multiplexer with CMake
7. Install FFmpeg runtime
8. Set entrypoint to multiplexer binary

**FFmpeg Containers**:
- Use `jrottenberg/ffmpeg:latest` or official FFmpeg image
- Override command via docker-compose

## Error Handling & Resilience

### UDP Socket Failures
- Retry socket binding with exponential backoff
- Log errors but continue attempting
- Allow other source to continue working

### FFmpeg Crash Recovery
- Detect process exit via `waitpid()` or pipe write failure
- Wait 1s, then restart FFmpeg
- Implement max restart attempts
- Log all RTMP connection issues

### Timestamp Anomalies
- Detect and handle PTS/DTS rollovers (33-bit wraparound)
- Skip packets with invalid timestamps
- Maintain reasonable timestamp deltas

### Queue Overflow
- Implement max queue size
- Drop oldest packets if queue full (log warning)
- Prevents unbounded memory growth

## Logging Strategy

**Log Levels**: DEBUG, INFO, WARN, ERROR

**Key Log Points**:
- Startup configuration values
- First detected PIDs from each source
- Mode transitions: "Switching to FALLBACK (no live for 2500ms)"
- Timestamp adjustments: Every 1000 packets or on source switch
- FFmpeg process status: started, crashed, restarted
- UDP socket errors
- PID remapping decisions

**Format**: ISO 8601 timestamp + level + component + message
```
2025-11-22T14:30:00.123Z [INFO] [Config] Loaded: live_port=10000, fallback_port=10001
2025-11-22T14:30:01.456Z [INFO] [Analyzer] Live PIDs: Video=256 Audio=257
2025-11-22T14:31:05.789Z [WARN] [Switcher] Mode: LIVE → FALLBACK (gap=2100ms)
```

## Build & Deployment

### Local Build
```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
./ts-multiplexer config.yaml
```

### Docker Build
```bash
docker-compose build
docker-compose up
```

### Testing Strategy
1. Start system: `docker-compose up`
2. Send SRT stream to localhost:1937
3. Verify RTMP output shows live content
4. Stop SRT stream
5. Verify automatic switch to fallback within 2s
6. Resume SRT stream
7. Verify switch back to live
8. Check RTMP continuity (no black frames, no timestamp errors)

## Performance Considerations

- **Memory**: Each packet is 188 bytes; queue size of 1000 packets = ~188 KB per queue
- **CPU**: TSDuck operations are lightweight; main cost is timestamp calculation
- **Latency**: Target <100ms end-to-end from UDP input to RTMP output
- **Throughput**: Typical TS bitrate 2-8 Mbps; pacing ensures real-time delivery

## Future Enhancements

1. Support for multiple audio tracks
2. DVB subtitles preservation
3. Web UI for monitoring mode/status
4. Metrics export (Prometheus)
5. Configurable switch-back delay (prevent flapping)
6. SCTE-35 ad insertion point handling
7. Multiple fallback sources with priority