# SRT Switcher v4.0 - Simplified Dual-FFmpeg Architecture
# SRT Switcher v5.0 - Low-Latency Dual-FFmpeg Architecture

## Overview

This is a **dual-FFmpeg + GStreamer hybrid architecture** that uses:
1. FFmpeg for input normalization (offline video and SRT)
2. GStreamer for seamless stream switching with FLV demuxing/muxing
3. GStreamer's native rtmp2sink for reliable RTMP streaming to Kick

This approach eliminates the problematic third FFmpeg output process while providing robust, well-monitored RTMP streaming with GStreamer's native implementation.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│ FFmpeg Process 1: Offline Video (Always Running)                │
│                                                                  │
│  ┌──────────┐    ┌──────────┐    ┌─────────┐    ┌──────────┐  │
│  │ File     │ →  │ Decode   │ →  │ Encode  │ →  │ FLV Mux  │  │
│  │ Loop     │    │ (any fmt)│    │ H264+AAC│    │          │  │
│  └──────────┘    └──────────┘    └─────────┘    └────┬─────┘  │
│                                                        │stdout   │
└────────────────────────────────────────────────────────┼────────┘
                                                         │
                                                         ▼ FD pipe
┌─────────────────────────────────────────────────────────────────┐
│ GStreamer Pipeline (Switcher with Direct RTMP Output)          │
│                                                                  │
│  ┌────────┐    ┌─────────┐                                     │
│  │ fdsrc  │ →  │flvdemux │ ─┐                                  │
│  └────────┘    └─────────┘  │                                  │
│  (offline)                   ├→ video pad                       │
│                              └→ audio pad                       │
│                                   │                             │
│                                   ▼                             │
│                            ┌──────────────┐                     │
│                            │input-selector│                     │
│                            │   (video)    │                     │
│                            └──────┬───────┘                     │
│  ┌────────┐    ┌─────────┐       │                             │
│  │ fdsrc  │ →  │flvdemux │ ─┐    │                             │
│  └────────┘    └─────────┘  │    │                             │
│  (srt)                       ├────┘                             │
│                              │                                  │
│                              └→ audio pad                       │
│                                   │                             │
│                                   ▼                             │
│                            ┌──────────────┐                     │
│                            │input-selector│                     │
│                            │   (audio)    │                     │
│                            └──────┬───────┘                     │
│                                   │                             │
│                                   ▼                             │
│                              ┌─────────┐                        │
│                              │ flvmux  │                        │
│                              └────┬────┘                        │
│                                   │                             │
│                                   ▼                             │
│                             ┌──────────┐                        │
│                             │rtmp2sink │                        │
│                             └────┬─────┘                        │
│                                  │                              │
└──────────────────────────────────┼──────────────────────────────┘
                                   │ RTMP/TCP
                                   ▼
                          ┌────────────────┐
                          │  Kick Server   │
                          └────────────────┘
                                   ▲ FD pipe
                                   │stdout
┌────────────────────────────────────┼──────────────────────────┐
│ FFmpeg Process 2: SRT Input (Always Running)                  │
│                                    │                           │
│  ┌──────────┐    ┌──────────┐    ┌┴───────┐    ┌─────────┐  │
│  │ SRT      │ →  │ Decode   │ →  │ Encode │ →  │ FLV Mux │  │
│  │ Listener │    │ (any fmt)│    │H264+AAC│    │         │  │
│  └──────────┘    └──────────┘    └────────┘    └─────────┘  │
│  :9000                                                         │
└────────────────────────────────────────────────────────────────┘
```

## Key Components

### 1. FFmpeg Normalization Processes (Inputs Only)

**Purpose:** Convert any input format to consistent H.264+AAC FLV streams

#### Offline Video Process
- **Input:** Local video file (any format supported by FFmpeg)
- **Features:**
  - Infinite looping (`-stream_loop -1`)
  - Real-time playback (`-re`)
  - Consistent output format
- **Output:** FLV stream to stdout (pipe:1)

#### SRT Input Process
- **Input:** SRT listener on port 9000 (UDP)
- **Features:**
  - Wait for connections (listener mode)
  - Automatic reconnection handling
  - Same normalization as offline
- **Output:** FLV stream to stdout (pipe:1)

**Normalization Parameters:**
- Video: H.264 (libx264), 1920x1080@30fps, 3000kbps
- Audio: AAC, 48kHz stereo, 128kbps
- Keyframes: Every 2 seconds for seamless switching (v4.0)
- Keyframes: Every 0.5 seconds for fast switching (v5.0, configurable)
- Buffer: 1x bitrate (reduced from 2x in v5.0)
- Preset: veryfast (configurable)
- Tune: zerolatency (low latency encoding)

#### Low-Latency Optimizations (v5.0+)

**SRT Input Optimizations:**
- `-fflags nobuffer+flush_packets`: Disable input buffering for immediate processing
- `-flags low_delay`: Enable low-delay encoding mode
- `-probesize 32`: Minimal stream probing (vs default 5MB)
- `-analyzeduration 0`: Skip stream analysis delay

**Benefits:**
- Reduces switching delay from 15-20s to 1-3s
- Maintains stability with balanced defaults
- Configurable via environment variables

**Trade-offs:**
- Faster keyframes (+10% bandwidth usage)
- Less input buffering (requires stable SRT connection)
- Smaller queues (less jitter tolerance)

### 2. GStreamer Pipeline with Direct RTMP Output

**Purpose:** Switch between pre-normalized FLV streams and output directly to RTMP

#### Pipeline Elements

1. **fdsrc (offline):** Reads offline FLV from FFmpeg stdout
2. **fdsrc (srt):** Reads SRT FLV from FFmpeg stdout
3. **flvdemux (x2):** Demuxes FLV into separate video and audio streams
4. **input-selector (video):** Switches between video streams
5. **input-selector (audio):** Switches between audio streams
6. **flvmux:** Re-muxes selected streams into FLV format
7. **rtmp2sink:** Native GStreamer RTMP output to Kick

**Why Native GStreamer RTMP?**
- **Simpler architecture:** No third FFmpeg process needed
- **Direct control:** Enable/disable via location property
- **Bus messages:** Connection status visible in GStreamer logs
- **No stuck states:** No pipe buffering issues
- **Reliable:** GStreamer's rtmp2sink is mature and stable

#### Kick Streaming Control

**Enable Streaming:**
```python
rtmp2sink.set_property("location", "rtmps://kick.com/app/key")
```

**Disable Streaming:**
```python
rtmp2sink.set_property("location", "")
```

This is much simpler than managing a third FFmpeg process and valve mechanism!

### 3. Process Management

#### FFmpegProcessManager
- Manages two FFmpeg subprocesses (offline + SRT)
- Monitors stdout/stderr for both processes
- Handles process crashes/restarts
- Provides file descriptors to GStreamer

#### SRTMonitor Thread
- Tracks data flow timing
- Detects SRT connection/disconnection
- Triggers automatic switching (camera mode)
- Debounce logic (2.5s timeout)

## Data Flow

### Normal Operation

1. **Startup:**
   - FFmpeg offline process starts, begins looping video
   - FFmpeg SRT process starts, waits for connection
   - GStreamer pipeline reads from input FDs
   - flvdemux splits streams into video/audio
   - input-selectors default to offline pads
   - Stream flows: offline → demux → selectors → flvmux → rtmp2sink (disabled)

2. **Enable Kick Streaming:**
   - User calls POST /kick/start
   - rtmp2sink location property set to Kick RTMP URL
   - rtmp2sink connects and begins pushing stream
   - RTMP connection status visible in bus messages

3. **SRT Connection:**
   - FFmpeg SRT receives connection
   - Data flows through SRT stdout
   - SRTMonitor detects data flow
   - `_on_srt_connected()` callback triggers
   - input-selectors switch to SRT pads (both video and audio)
   - Stream flows: srt → demux → selectors → flvmux → rtmp2sink → Kick

4. **SRT Disconnection:**
   - No data for 2.5 seconds
   - SRTMonitor detects timeout
   - `_on_srt_disconnected()` callback triggers
   - input-selectors switch to offline pads
   - Stream flows: offline → demux → selectors → flvmux → rtmp2sink → Kick

5. **Disable Kick Streaming:**
   - User calls POST /kick/stop
   - rtmp2sink location property cleared
   - RTMP connection closes gracefully
   - Pipeline continues running (ready to restart)

### Scene Modes

**Camera Mode (Auto-switching):**
- Automatically switch to SRT when connected
- Automatically fallback to offline when disconnected
- Real-time monitoring with 2.5s debounce

**Privacy Mode (Always offline):**
- Force offline video regardless of SRT
- SRT connection still monitored but not used
- Useful for "BRB" or "Away" scenarios

## Benefits vs v3.0 (Three FFmpeg Processes)

### Reliability
- ✅ **No stuck FFmpeg processes:** Eliminated third FFmpeg output process
- ✅ **No pipe buffering issues:** Direct GStreamer RTMP output
- ✅ **Visible RTMP status:** GStreamer bus messages show connection state
- ✅ **Clean enable/disable:** Simple property change, no process management

### Performance
- ✅ **Lower CPU usage:** One fewer FFmpeg process (~20-30% reduction)
- ✅ **Lower memory usage:** Fewer pipes and buffers
- ✅ **Same encoding quality:** Input normalization unchanged

### Debugging
- ✅ **Clear logs:** GStreamer bus messages for RTMP status
- ✅ **Simpler architecture:** Fewer moving parts to debug
- ✅ **No valve complexity:** Direct flow control via location property

### Flexibility
- ✅ **Easy restarts:** Simply change location property
- ✅ **No stuck states:** GStreamer handles RTMP reconnection
- ✅ **Extensible:** Can add recording, HLS, etc. via tee element

## Configuration

### Environment Variables

```bash
# SRT Settings
SRT_PORT=9000                 # SRT listener port
SRT_TIMEOUT=2.5              # Seconds before considering disconnected

# Video Settings
FALLBACK_VIDEO=/videos/fallback.mp4
OUTPUT_WIDTH=1920
OUTPUT_HEIGHT=1080
OUTPUT_FPS=30
OUTPUT_BITRATE=3000          # Video bitrate in kbps

# FFmpeg Encoding
FFMPEG_PRESET=veryfast       # ultrafast|superfast|veryfast|faster|fast|medium
FFMPEG_TUNE=zerolatency      # zerolatency|film|animation
FFMPEG_AUDIO_BITRATE=128     # Audio bitrate in kbps

# Output (RTMP)
KICK_URL=rtmps://your-server/app
KICK_KEY=your-stream-key

# API
API_PORT=8088

# Low-Latency Switching (v5.0+)
LOW_LATENCY_MODE=true        # Enable low-latency optimizations
SRT_MONITOR_INTERVAL=0.5     # Monitor check interval (seconds)
QUEUE_BUFFER_SIZE=3          # GStreamer queue size (buffers)
GOP_DURATION=0.5             # Keyframe interval (seconds)
```

## File Structure

```
srt-switcher/
├── ffmpeg_process.py          # FFmpeg subprocess management (2 processes)
├── gstreamer_simple.py        # GStreamer pipeline with rtmp2sink
├── stream_switcher.py         # Main coordinator
├── srt_monitor.py             # SRT connection monitoring
├── config.py                  # Configuration management
├── api_server.py              # HTTP API server
├── switcher.py                # Entry point
├── Dockerfile                 # Container build
└── ARCHITECTURE.md            # This file
```

## API Endpoints

### Health Check
```bash
GET /health
# Returns: status, current_source, srt_connected, kick_streaming_enabled, ffmpeg_processes, etc.
```

### Manual Switching
```bash
GET /switch?src=srt      # Switch to SRT
GET /switch?src=offline  # Switch to offline
```

### Scene Control
```bash
POST /scene/camera   # Enable auto-switching
POST /scene/privacy  # Force offline video
```

### Kick Streaming
```bash
POST /kick/start    # Enable RTMP streaming (set location)
POST /kick/stop     # Disable RTMP streaming (clear location)
```

## Troubleshooting

### FFmpeg Process Issues

**Check process status:**
```bash
curl http://localhost:8088/health | jq '.ffmpeg_processes'
```

**Check FFmpeg logs:**
```bash
docker compose logs srt-switcher | grep ffmpeg
```

**Common issues:**
- **"Failed to start FFmpeg":** Check video file exists and is readable
- **"No such file or directory":** Verify fallback video path
- **"Codec not found":** FFmpeg may need rebuild with specific codec support

### RTMP Connection Issues

**Check rtmp2sink status in logs:**
```bash
docker compose logs srt-switcher | grep -i rtmp
```

**Look for these indicators:**
- **"[RTMP ERROR]":** Connection problem (URL, key, network)
- **"[RTMP] kick-sink":** Element messages about connection state
- **GStreamer element messages:** Detailed RTMP status updates

**Common solutions:**
- Verify KICK_URL and KICK_KEY are correct
- Check network connectivity to Kick servers
- Ensure firewall allows outbound RTMP connections
- Monitor GStreamer bus messages for detailed error information

### GStreamer Issues

**Check pipeline state:**
```bash
curl http://localhost:8088/health | jq '.pipeline_state'
```

**Common issues:**
- **"Pipeline stuck in PAUSED":** Check FFmpeg input processes are running
- **"No data flowing":** Verify file descriptors are valid
- **"flvdemux error":** FFmpeg output may not be valid FLV format

## Performance Tuning

### CPU Usage
- **Lower preset:** `FFMPEG_PRESET=faster` (more CPU, better quality)
- **Higher preset:** `FFMPEG_PRESET=ultrafast` (less CPU, lower quality)

### Quality vs Latency
- **Lower latency:** `FFMPEG_TUNE=zerolatency` (current default)
- **Better quality:** `FFMPEG_TUNE=film` (slightly higher latency)

### Bitrate Optimization
- **720p streaming:** `OUTPUT_BITRATE=2000`
- **1080p streaming:** `OUTPUT_BITRATE=3000` (current)
- **Higher quality:** `OUTPUT_BITRATE=5000` (more bandwidth)

## Migration from v3.0

1. **No code changes needed in API clients**
2. **Same environment variables** (KICK_URL and KICK_KEY)
3. **Rebuild container:** `docker compose build srt-switcher`
4. **Restart service:** `docker compose up -d srt-switcher`
5. **Monitor logs:** Watch for successful RTMP connection!

## Future Enhancements

- [ ] Automatic quality adaptation based on bandwidth
- [ ] Multiple SRT input support (source selection)
- [ ] Recording capability via tee to filesink
- [ ] HLS output for browser playback via tee to hlssink
- [ ] Metrics/statistics for RTMP connection
- [ ] Multiple RTMP destinations via tee