# SRT Switcher v3.0 - Quick Start Guide

## What's New in v3.0

**Hybrid FFmpeg+GStreamer Architecture:**
- ✅ Uses FFmpeg for input normalization (eliminates caps negotiation errors)
- ✅ Simplified GStreamer pipeline (just switching, no encoding)
- ✅ Works with ANY video format
- ✅ More reliable and easier to debug
- ✅ Same API as v2.0 (no breaking changes)

## Quick Setup

### 1. Build and Start

```bash
cd srt-switcher
docker compose build srt-switcher
docker compose up -d srt-switcher
```

### 2. Check Status

```bash
# Wait 5-10 seconds for startup, then check health
curl http://localhost:9088/health | jq
```

**Expected output:**
```json
{
  "status": "healthy",
  "current_source": "offline",
  "srt_connected": false,
  "pipeline_state": "playing",
  "ffmpeg_processes": {
    "offline": {
      "running": true,
      "pid": 123
    },
    "srt": {
      "running": true,
      "pid": 124
    }
  }
}
```

### 3. Verify Offline Video

```bash
# Check logs for FFmpeg offline process
docker compose logs srt-switcher | grep ffmpeg-offline

# Should see:
# [ffmpeg-offline] FFmpeg started (PID: xxx)
# [ffmpeg-offline] frame=...
```

### 4. Test SRT Input

**Send test stream with FFmpeg:**
```bash
# Generate test pattern
ffmpeg -re -f lavfi -i testsrc=size=1920x1080:rate=30 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -c:v libx264 -preset veryfast -b:v 2000k \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:9937?mode=caller"
```

**Or from file:**
```bash
ffmpeg -re -i your_video.mp4 \
  -c:v libx264 -preset veryfast -b:v 2000k \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:9937?mode=caller"
```

### 5. Verify Auto-Switching

```bash
# Check that switcher detected SRT
curl http://localhost:9088/health | jq '.current_source, .srt_connected'

# Should show:
# "srt"
# true
```

**In logs:**
```bash
docker compose logs -f srt-switcher | grep -E "srt|switch"

# Should see:
# [monitor] ✓ SRT connection detected
# [auto] SRT feed connected (mode=camera)
# [switch] ✓ Active source: SRT
```

### 6. Test Disconnection

```bash
# Stop FFmpeg test stream (Ctrl+C)
# Wait 3 seconds

curl http://localhost:9088/health | jq '.current_source, .srt_connected'

# Should show:
# "offline"
# false
```

## Quick Troubleshooting

### FFmpeg Offline Not Working

```bash
# 1. Check offline FFmpeg process
docker compose logs srt-switcher | grep "ffmpeg-offline"

# 2. Check video file exists
docker exec srt-switcher ls -lh /videos/

# 3. Test video file directly
docker exec srt-switcher ffmpeg -i /videos/fallback.mp4 -t 5 -f null -

# 4. If file is bad, replace it
docker cp good-video.mp4 srt-switcher:/videos/fallback.mp4
docker compose restart srt-switcher
```

### SRT Not Connecting

```bash
# 1. Check SRT FFmpeg process
docker compose logs srt-switcher | grep "ffmpeg-srt"

# 2. Verify port is exposed
docker compose ps srt-switcher

# 3. Check from host
netstat -an | grep 9937

# 4. Test with verbose output
ffmpeg -re -i test.mp4 -c copy -f mpegts "srt://localhost:9937?mode=caller" -v verbose
```

### Pipeline Not Starting

```bash
# 1. Check pipeline state
curl http://localhost:9088/health | jq '.pipeline_state'

# 2. Check for GStreamer errors
docker compose logs srt-switcher | grep ERROR

# 3. Verify FFmpeg processes have valid FDs
curl http://localhost:9088/health | jq '.ffmpeg_processes'

# 4. Restart if needed
docker compose restart srt-switcher
```

## Configuration Tips

### Lower CPU Usage

```yaml
# In docker-compose.yml or .env
environment:
  FFMPEG_PRESET: ultrafast  # Default: veryfast
  OUTPUT_BITRATE: 2000      # Default: 3000
```

### Better Quality

```yaml
environment:
  FFMPEG_PRESET: faster     # Slower but better quality
  OUTPUT_BITRATE: 5000      # Higher bitrate
  FFMPEG_AUDIO_BITRATE: 192 # Better audio
```

### Different Resolution

```yaml
environment:
  OUTPUT_WIDTH: 1280
  OUTPUT_HEIGHT: 720
  OUTPUT_FPS: 60           # For smoother motion
  OUTPUT_BITRATE: 4000     # Adjust for resolution/fps
```

### Low-Latency Switching (v5.0+)

**Fast Switching (1-3 seconds):**
```yaml
environment:
  LOW_LATENCY_MODE: true           # Enable optimizations (default: true)
  SRT_MONITOR_INTERVAL: 0.5        # Check interval in seconds (default: 0.5)
  QUEUE_BUFFER_SIZE: 3             # GStreamer queue size (default: 3)
  GOP_DURATION: 0.5                # Keyframe interval in seconds (default: 0.5)
```

**What These Do:**
- `LOW_LATENCY_MODE`: Enables FFmpeg input buffering optimizations
- `SRT_MONITOR_INTERVAL`: How often to check for SRT connection (lower = faster detection)
- `QUEUE_BUFFER_SIZE`: Smaller queues = less latency (but less buffer for jitter)
- `GOP_DURATION`: Faster keyframes = quicker switching (but +10% bandwidth)

**Trade-offs:**
- Lower values = faster switching but less tolerance for network jitter
- Default values (0.5s interval, 3 buffers, 0.5s GOP) balance speed and stability
- For ultra-stable networks, try `SRT_MONITOR_INTERVAL: 0.25` and `QUEUE_BUFFER_SIZE: 2`
- For unstable networks, increase to `SRT_MONITOR_INTERVAL: 1.0` and `QUEUE_BUFFER_SIZE: 5`

## API Quick Reference

### Switch Sources Manually
```bash
curl "http://localhost:9088/switch?src=srt"
curl "http://localhost:9088/switch?src=offline"
```

### Scene Control
```bash
# Auto-switching (default)
curl "http://localhost:9088/scene?mode=camera"

# Always offline (privacy mode)
curl "http://localhost:9088/scene?mode=privacy"
```

### Kick Streaming
```bash
# Enable streaming
curl -X POST http://localhost:9088/kick/enable

# Disable streaming
curl -X POST http://localhost:9088/kick/disable
```

## Performance Monitoring

### CPU Usage
```bash
docker stats srt-switcher --no-stream

# Typical usage:
# - FFmpeg offline: 20-30% CPU
# - FFmpeg SRT: 20-30% CPU
# - GStreamer: 10-15% CPU
# Total: 50-75% CPU (acceptable for streaming)
```

### Memory Usage
```bash
docker stats srt-switcher --no-stream

# Typical: 200-400 MB RAM
```

### Check FFmpeg Process Health
```bash
# View FFmpeg process status in detail
curl http://localhost:9088/health | jq '.ffmpeg_processes'

# Check if processes are alive
docker exec srt-switcher ps aux | grep ffmpeg
```

## Common Scenarios

### Scenario 1: Just Test Offline Video
```bash
# Start container
docker compose up -d srt-switcher

# Wait 10 seconds
sleep 10

# Should be playing offline video
curl http://localhost:9088/health | jq '.current_source'
# Output: "offline"
```

### Scenario 2: Quick SRT Test
```bash
# Terminal 1: Start switcher
docker compose up -d srt-switcher

# Terminal 2: Send test stream
ffmpeg -re -f lavfi -i testsrc2=size=1920x1080:rate=30 \
  -f lavfi -i sine=frequency=440 \
  -c:v libx264 -preset ultrafast -b:v 1500k \
  -c:a aac -b:a 96k \
  -f mpegts "srt://localhost:9937?mode=caller"

# Terminal 3: Watch logs
docker compose logs -f srt-switcher | grep -E "monitor|switch|srt"
```

### Scenario 3: Privacy Mode (BRB Screen)
```bash
# Set to privacy mode (always offline)
curl "http://localhost:9088/scene?mode=privacy"

# Verify - should show offline even if SRT connected
curl http://localhost:9088/health | jq '.scene_mode, .current_source'
# Output: "privacy", "offline"

# Return to normal
curl "http://localhost:9088/scene?mode=camera"
```

## Next Steps

1. **Read full architecture:** See [`ARCHITECTURE.md`](ARCHITECTURE.md)
2. **API documentation:** See [`README.md`](README.md#api-endpoints)
3. **Advanced testing:** See [`TESTING.md`](TESTING.md)
4. **Production deployment:** Configure `KICK_URL` and `KICK_KEY` in `.env`

## Getting Help

**Check logs:**
```bash
docker compose logs srt-switcher
```

**Restart clean:**
```bash
docker compose down
docker compose up -d srt-switcher
docker compose logs -f srt-switcher
```

**Report issues with:**
- Health check output: `curl http://localhost:9088/health`
- FFmpeg process status
- Recent log output (last 50 lines)
- Your video file format: `ffprobe /path/to/video.mp4`