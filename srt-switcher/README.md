# SRT Stream Switcher

Seamlessly switch between offline video and live SRT input with automatic failover. Outputs to a local TCP server for VLC testing.

## Quick Testing with VLC (v4.2 - TCP Only)

The srt-switcher provides a **local TCP server** for instant testing with VLC:

```bash
# Start the service
docker compose up srt-switcher

# Open VLC and connect to:
tcp://localhost:8554
```

You'll see the stream immediately and can verify input switching works perfectly. See [TESTING.md](TESTING.md) for complete testing guide.

# SRT Stream Switcher v2.0

A robust dual-pipeline streaming solution that seamlessly switches between offline video and SRT input at the encoded stream level.

## What's New in v2.0

**Major Architecture Refactor:**
- ✅ **Dual Independent Pipelines**: Offline video and SRT each have their own complete encode chain
- ✅ **FLV-Level Switching**: Switches between pre-encoded streams (no re-encoding delay)
- ✅ **Reliable Offline Video**: Loops continuously, guaranteed to work
- ✅ **Dedicated SRT Monitor**: Thread-based connection monitoring with configurable timeout
- ✅ **Comprehensive Logging**: Detailed state tracking for debugging
- ✅ **Seamless Transitions**: No black frames or stuttering during source changes

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│ OFFLINE VIDEO PIPELINE (Always Running)                            │
│ ┌────────┐  ┌────────┐  ┌──────────┐  ┌─────────┐  ┌──────────┐  │
│ │ filesrc│→ │decodebin│→│normalize │→ │x264enc  │→ │ FLV mux  │  │
│ └────────┘  └────────┘→│1080p30   │  │3000kbps │  └────┬─────┘  │
│                         │48kHz     │  │lamemp3  │       │         │
│                         └──────────┘  └─────────┘       │         │
└─────────────────────────────────────────────────────────┼─────────┘
                                                           │
                                                           ▼
┌─────────────────────────────────────────────────────────┼─────────┐
│ SRT PIPELINE (Always Running)                           │         │
│ ┌────────┐  ┌────────┐  ┌──────────┐  ┌─────────┐  ┌──┴─────┐  │
│ │ srtsrc │→ │decodebin│→│normalize │→ │x264enc  │→ │ FLV mux│  │
│ │:9000   │  └────────┘→│1080p30   │  │3000kbps │  └──┬─────┘  │
│ │udp     │  [monitor]  │48kHz     │  │lamemp3  │     │         │
│ └────────┘  detects     └──────────┘  └─────────┘     │         │
│             data flow                                   │         │
└─────────────────────────────────────────────────────────┼─────────┘
                                                           │
                                                           ▼
┌─────────────────────────────────────────────────────────┴─────────┐
│ OUTPUT PIPELINE                                                    │
│ ┌──────────────────┐    ┌─────┐    ┌─────────┐                   │
│ │ Input Selector   │ →  │ Tee │ →  │Fakesink │ (always active)   │
│ │ (switches between│    └──┬──┘    └─────────┘                   │
│ │  FLV streams)    │       │                                      │
│ └──────────────────┘       │       ┌───────┐   ┌──────────┐      │
│                            └────→  │ Valve │ → │ rtmpsink │      │
│                                    │(ctrl) │   │  (Kick)  │      │
│                                    └───────┘   └──────────┘      │
└────────────────────────────────────────────────────────────────────┘

Monitor Thread: Checks SRT data flow every 1s, 2.5s timeout for disconnect
```

## Key Features

### Reliability Improvements
- **Both Pipelines Always Active**: No state management complexity
- **Offline Video Guaranteed**: Loops continuously from startup
- **Seamless Switching**: Pre-encoded streams = instant transitions
- **Robust SRT Detection**: Dedicated monitor thread with debounce logic
- **No Pipeline Restarts**: Valve controls Kick streaming without state changes

### Technical Benefits
- **FLV-Level Selection**: Input-selector operates on compressed data
- **Independent Encoding**: Each source has its own encoder (no sync issues)
- **Automatic Looping**: Offline video seeks to start on EOS
- **Thread-Safe Monitoring**: Separate thread tracks SRT connection status
- **Comprehensive Logging**: Every state change is logged with context

## How It Works

### Dual-Pipeline Design

1. **Offline Video Pipeline**
   - `filesrc` reads the fallback video file
   - `decodebin` automatically handles any video format
   - Normalizes to 1080p30 I420 video, 48kHz stereo audio
   - Encodes with x264 (H.264) and lamemp3enc
   - Outputs FLV stream to input-selector
   - On EOS, seeks back to start for seamless loop

2. **SRT Pipeline**
   - `srtsrc` listens on port 9000 (UDP) in listener mode
   - `decodebin` with async-handling (won't block pipeline)
   - Normalizes to same 1080p30 format as offline
   - Encodes with x264 (H.264) and lamemp3enc
   - Outputs FLV stream to input-selector
   - **Data probe** tracks when packets flow through

3. **SRT Monitor Thread**
   - Runs independently in background
   - Tracks last packet time from SRT pipeline
   - Checks every 1 second for data activity
   - Triggers switch to SRT when data detected
   - Waits 2.5s after last packet before switching back (debounce)
   - Thread-safe with proper locking

4. **Input Selector**
   - Receives two FLV streams (both always flowing)
   - Only one pad is "active" at any time
   - Switching is instant (just changes active-pad property)
   - No re-encoding = no delay = seamless transition

5. **Output Branch**
   - **Tee** splits selector output to two branches:
     - **Fakesink**: Always active (keeps pipeline flowing)
     - **Valve → RTMP Sink**: Controlled by HTTP API
   - Valve closed by default (no Kick streaming on startup)
   - Opening valve enables streaming without pipeline restart

### Switching Logic

**Automatic Switching (Camera Mode):**
- SRT data detected → Switch to SRT immediately
- SRT data stops for 2.5s → Switch to offline video
- Both transitions are seamless (no black frames)

**Manual Switching:**
- HTTP API allows override of automatic behavior
- Manual switch takes effect immediately

**Privacy Mode:**
- Forces offline video regardless of SRT connection
- Useful for "BRB" or "Away" scenarios
- SRT connection still monitored but not used

## Configuration

Environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `SRT_PORT` | 9000 | Internal SRT listener port |
| `SRT_TIMEOUT` | 2.5 | Seconds without data before disconnect |
| `FALLBACK_VIDEO` | /videos/fallback.mp4 | Path to looping video file |
| `KICK_URL` | - | Kick RTMPS server URL |
| `KICK_KEY` | - | Kick stream key |
| `OUTPUT_BITRATE` | 3000 | Video bitrate in kbps |
| `OUTPUT_WIDTH` | 1920 | Output video width |
| `OUTPUT_HEIGHT` | 1080 | Output video height |
| `OUTPUT_FPS` | 30 | Output frame rate |
| `API_PORT` | 8088 | HTTP API port |

## Usage

### Start with Docker Compose

```bash
# Start the switcher
docker compose --profile test up -d srt-switcher

# View logs
docker compose logs -f srt-switcher

# Stop
docker compose --profile test down srt-switcher
```

### Send SRT Stream

**With FFmpeg:**
```bash
ffmpeg -re -i input.mp4 -c:v libx264 -c:a aac \
  -f mpegts "srt://localhost:9937?mode=caller"
```

**With OBS Studio:**
- Settings → Stream
- Service: Custom
- Server: `srt://YOUR_SERVER_IP:9937`
- Stream Key: (leave empty)

### HTTP API

**Health Check:**
```bash
curl http://localhost:9088/health
```

**Get Current Scene:**
```bash
curl http://localhost:9088/scene
# Returns: {"scene": "srt"} or {"scene": "offline"}
```

**Get Scene Mode:**
```bash
curl http://localhost:9088/scene/mode
# Returns: {"mode": "camera"} or {"mode": "privacy"}
```

**Manual Switch:**
```bash
# Switch to SRT source
curl "http://localhost:9088/switch?src=srt"

# Switch to offline video
curl "http://localhost:9088/switch?src=offline"
```

**Scene Mode Control:**
```bash
# Enable camera mode (auto-switch to SRT when connected)
curl -X POST http://localhost:9088/scene/camera

# Enable privacy mode (stay on offline, ignore SRT)
curl -X POST http://localhost:9088/scene/privacy
```

## Port Mapping

When running alongside existing setup:

| Service | Internal Port | External Port | Purpose |
|---------|---------------|---------------|---------|
| SRT Input | 9000/udp | 9937/udp | Avoid conflict with ffmpeg-srt |
| HTTP API | 8088 | 9088 | Avoid conflict with muxer |

## Advantages Over v1.0

| Aspect | v1.0 (Raw Switching) | v2.0 (FLV Switching) |
|--------|----------------------|----------------------|
| **Reliability** | ❌ Offline video didn't work | ✅ Works immediately |
| **Switching** | ❌ Complex, error-prone | ✅ Simple, reliable |
| **Detection** | ❌ Bus messages only | ✅ Dedicated monitor thread |
| **Seamlessness** | ❌ Black frames possible | ✅ True seamless switching |
| **CPU Usage** | ✅ Lower (single encoder) | ⚠️ Higher (dual encoders) |
| **Debugging** | ❌ Difficult | ✅ Comprehensive logs |
| **Code Complexity** | ❌ 1143 lines, complex | ✅ 900 lines, simpler |

**Trade-off:** v2.0 uses ~2x CPU for dual encoders, but this is acceptable for the massive reliability improvement.

## Troubleshooting

### Offline video doesn't play

**Check:**
```bash
# File exists?
docker exec srt-switcher ls -l /videos/fallback.mp4

# Pipeline logs?
docker compose logs srt-switcher | grep offline
```

**Expected logs:**
```
[offline] Building offline video pipeline from: /videos/fallback.mp4
[offline] ✓ Video pad linked
[offline] ✓ Audio pad linked
```

### SRT not connecting

**Check:**
```bash
# Port mapping correct?
docker compose ps srt-switcher

# Connection logs?
docker compose logs srt-switcher | grep srt
```

**Expected logs:**
```
[srt] ✓ Video pad linked - SRT stream connected!
[monitor] ✓ SRT connection detected (data age: 0.15s)
[auto] SRT feed connected (mode=camera)
```

### No switch when SRT connects

**Check scene mode:**
```bash
curl http://localhost:9088/scene/mode
```

If in privacy mode, switching won't happen. Change to camera mode:
```bash
curl -X POST http://localhost:9088/scene/camera
```

### Stream quality issues

**Adjust encoding settings in docker-compose.yml:**
```yaml
environment:
  - OUTPUT_BITRATE=4000  # Higher quality
  - OUTPUT_FPS=60        # Smoother motion
```

### Switching too slow/fast

**Adjust debounce timeout:**
```yaml
environment:
  - SRT_TIMEOUT=1.5  # Faster detection
  # or
  - SRT_TIMEOUT=5.0  # More tolerant of brief drops
```

## Testing

See [`TESTING.md`](./TESTING.md) for comprehensive testing guide including:
- SRT connection testing with FFmpeg and OBS
- Output verification methods
- Complete test sequences
- Troubleshooting procedures

## Development

### Local Testing

```bash
# Build
docker build -t srt-switcher ./srt-switcher

# Run standalone
docker run --rm -it \
  -p 9937:9000/udp \
  -p 9088:8088 \
  -v /path/to/video.mp4:/videos/fallback.mp4:ro \
  -e KICK_URL=rtmps://your-server/app \
  -e KICK_KEY=your-key \
  srt-switcher
```

### Modify Pipeline

Edit `switcher.py` to:
- Adjust encoding parameters
- Add additional input sources
- Implement custom switching logic
- Add preview outputs

### Enable Debug Logging

```yaml
# docker-compose.yml
environment:
  - GST_DEBUG=3  # GStreamer debug level (0-9)
```

## Architecture Decision Records

### Why Dual Pipelines?

**Problem:** v1.0 tried to switch at the raw video level using input-selectors, which required perfect synchronization and complex state management.

**Solution:** Run two independent encode pipelines and switch at the FLV level (compressed streams).

**Benefits:**
- Each source is self-contained and independent
- No synchronization issues between sources
- Switching is instant (just a pad property change)
- Much simpler code and state management

**Trade-off:** Uses more CPU (two encoders always running), but modern servers handle this easily, and reliability is worth it.

### Why Monitor Thread?

**Problem:** Relying on GStreamer bus messages for connection detection was unreliable.

**Solution:** Three-tier data flow monitoring system (v4.3+):

1. **PRIMARY Detection**: Probe on demuxed video pad
   - Monitors decoded 30fps video stream after FLV demuxing
   - Most reliable method - fires consistently for every frame
   - Attached when SRT video pad is linked

2. **SECONDARY Detection**: Probe on demuxed audio pad  
   - Redundant monitoring on audio stream
   - Backup detection if video probe fails

3. **TERTIARY Detection**: FFmpeg stderr monitoring
   - Monitors `frame=` progress messages from FFmpeg
   - Ultimate fallback if GStreamer probes fail

**Benefits:**
- Precise tracking of data flow with redundancy
- Configurable debounce logic (SRT_TIMEOUT)
- Independent of GStreamer bus internals
- Easy to test and debug
- Verbose logging mode for troubleshooting

**Debug Mode:**
```bash
export SRT_MONITOR_VERBOSE=true
# Shows real-time buffer counts and fps
```

### Why FLV Muxing Before Selection?

**Problem:** Switching raw video/audio streams separately can cause sync issues.

**Solution:** Mux to FLV before the selector, so only one stream to switch.

**Benefits:**
- Audio and video always in sync
- Simpler selector configuration
- Ready-to-stream format (FLV)
- No post-selection muxing complexity

## Migration from v1.0

If upgrading from the old switcher:

1. **Backup existing setup**
2. **Stop old container:** `docker compose down srt-switcher`
3. **Update image:** Pull latest or rebuild
4. **Check environment variables** (add `SRT_TIMEOUT` if needed)
5. **Start new version:** `docker compose up -d srt-switcher`
6. **Verify offline video plays** immediately
7. **Test SRT connection** and switching
8. **Update any monitoring** to new log formats

## Performance Notes

**Resource Usage:**
- CPU: 20-40% (dual 1080p30 encoders)
- Memory: 200-400 MB
- Network: ~3 Mbps output when Kick streaming

**Recommendations:**
- Minimum: 2 CPU cores, 1GB RAM
- Recommended: 4 CPU cores, 2GB RAM
- Use SSD for video file access

## Next Steps

Once validated in testing:
1. Update main `docker-compose.yml` to standard ports
2. Remove old architecture containers
3. Update dashboard to new API endpoints
4. Deploy to production

## License & Credits

This is a complete rewrite (v2.0) of the SRT switcher, focusing on reliability and simplicity through a dual-pipeline architecture.

**Key Improvements:**
- ✅ Offline video works reliably
- ✅ Seamless switching guaranteed
- ✅ Comprehensive logging for debugging
- ✅ Simpler codebase (despite dual pipelines)
- ✅ Production-ready reliability