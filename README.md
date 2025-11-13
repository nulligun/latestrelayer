# SRT Stream Relay System

A simplified Docker-based streaming solution with automatic SRT failover to looping video and direct output to Kick.

## Architecture

```
┌─────────────────┐
│  SRT Source     │ (OBS, Camera, etc.)
│  Port 1937      │
└────────┬────────┘
         │
         ▼
┌─────────────────────────────────┐
│     srt-switcher                │
│  Single Python/GStreamer App    │
│                                 │
│  ┌─────────────┐               │
│  │ SRT Branch  │──┐            │
│  └─────────────┘  │            │
│                   ├─► Selector │
│  ┌─────────────┐  │      │     │
│  │ Fallback    │──┘      │     │
│  │ Video Loop  │         ▼     │
│  └─────────────┘    Encoder    │
│                        │        │
│  API: 8088            │        │
└────────────────────────┼────────┘
                         │
                         ▼
                  ┌──────────────┐
                  │ Kick Server  │
                  │   (RTMPS)    │
                  └──────────────┘

        ┌─────────────────┐
        │stream-controller│  ◄──┐
        │  Port: 8089     │     │
        └─────────────────┘     │
                                │
        ┌─────────────────┐     │
        │stream-dashboard │     │
        │  Port: 3000     │─────┘
        │  (Vue.js UI)    │
        └─────────────────┘
```

## Services

### 1. srt-switcher
- **Purpose**: Single-process streaming with automatic source switching
- **Technology**: Python 3 + GStreamer
- **Ports**: 1937 (SRT UDP), 8088 (HTTP API)
- **Features**:
  - SRT listener with automatic connection detection
  - Looping fallback video (brb.mp4)
  - Instant switching via input-selector
  - Direct encode to Kick RTMPS
  - Full audio/video synchronization
  - HTTP API for monitoring and control

### 2. stream-controller
- **Purpose**: Container lifecycle management via REST API
- **Port**: 8089
- **Capabilities**: Start/stop/restart/status for all containers

### 3. stream-dashboard
- **Purpose**: Web UI for monitoring and control
- **Port**: 3000 (HTTP + WebSocket)
- **Technology**: Vue.js 3 + Express + WebSocket
- **Features**:
  - Real-time system metrics
  - Stream status monitoring
  - Container controls
  - Live updates every 2 seconds

## Prerequisites

- Docker and Docker Compose
- Fallback video file (e.g., `/home/mulligan/offline.mp4`)
- Kick streaming credentials

## Configuration

Edit [`.env`](.env) file:

```bash
# Instance Identification
COMPOSE_PROJECT_NAME=relayer

# Kick Configuration
KICK_URL=rtmps://your-server.live-video.net/app
KICK_KEY=your_stream_key

# Ports
SRT_PORT=1937
MUXER_API_PORT=8088
CONTROLLER_API_PORT=8089
DASHBOARD_PORT=3000

# Fallback Video
BRB_VIDEO_PATH=/home/mulligan/offline.mp4

# Video Settings
OUT_RES=1080
OUT_FPS=30
VID_BITRATE=3000
```

## Usage

### Start All Services

```bash
docker compose up -d
```

### Send SRT Stream

**With FFmpeg:**
```bash
ffmpeg -re -i input.mp4 -c:v libx264 -c:a aac \
  -f mpegts "srt://localhost:1937?mode=caller"
```

**With OBS Studio:**
- Settings → Stream
- Service: Custom
- Server: `srt://YOUR_SERVER_IP:1937`
- Stream Key: (leave empty)

### Monitor Services

**View logs:**
```bash
# All services
docker compose logs -f

# Specific service
docker compose logs -f srt-switcher
```

**Stream switcher health:**
```bash
curl http://localhost:8088/health
```

**Get current scene:**
```bash
curl http://localhost:8088/scene
```

**Manual switch:**
```bash
# Switch to SRT source
curl "http://localhost:8088/switch?src=srt"

# Switch to fallback
curl "http://localhost:8088/switch?src=fallback"
```

**Web Dashboard:**
```bash
# Open in browser
http://localhost:3000
```

### Container Control API

**Start/stop/restart containers:**
```bash
# Start a container
curl -X POST "http://localhost:8089/container/srt-switcher/start"

# Stop a container
curl -X POST "http://localhost:8089/container/srt-switcher/stop"

# Restart a container
curl -X POST "http://localhost:8089/container/srt-switcher/restart"

# Check status
curl "http://localhost:8089/container/srt-switcher/status"

# List all containers
curl "http://localhost:8089/containers"
```

### Stop Services

```bash
docker compose down
```

## How It Works

### SRT Switcher Pipeline

The `srt-switcher` service runs a single GStreamer pipeline with two input branches:

1. **SRT Listener Branch**
   - `srtsrc` listens on port 9000 (internal, mapped to 1937 externally)
   - `decodebin` automatically detects codec
   - Normalizes to 1080p30, I420 format
   - When connection detected → auto-switch to this source

2. **Fallback Loop Branch**
   - `filesrc` reads the mounted video file
   - Continuously loops via seek on EOS
   - Same normalization to 1080p30
   - Active when no SRT connection

3. **Input Selector**
   - Both branches feed into video and audio selectors
   - Instant switching without pipeline restart
   - No dropped frames during transition

4. **Output Encoding**
   - x264 encoder: 3000kbps bitrate, zerolatency tune
   - AAC audio: 128kbps, 48kHz stereo
   - FLV container for RTMP/RTMPS
   - Direct push to Kick

### Auto-Switching Logic

- **SRT Connects**: Detected via `pad-added` signal → switch to SRT
- **SRT Disconnects**: Detected via ERROR message → switch to fallback
- **Fallback Loops**: On EOS event → seek back to timestamp 0
- **Manual Override**: HTTP API allows forcing specific source

## Stream Specifications

- **Resolution**: 1920x1080 (1080p)
- **Frame Rate**: 30 fps
- **Video Codec**: H.264 (x264)
- **Video Bitrate**: 3000 kbps
- **Audio Codec**: AAC
- **Audio Bitrate**: 128 kbps
- **Audio Sample Rate**: 48000 Hz
- **Audio Channels**: 2 (stereo)

## Troubleshooting

### SRT switcher won't start

```bash
# Check logs
docker compose logs srt-switcher

# Common issues:
# - Fallback video not found → check BRB_VIDEO_PATH
# - Port conflict → ensure 1937 and 8088 are free
# - Missing Kick credentials → verify KICK_URL and KICK_KEY
```

### SRT stream not connecting

```bash
# Verify port is open
nc -zvu localhost 1937

# Check for connection in logs
docker compose logs -f srt-switcher | grep srt

# Test with simple source
ffplay -f mpegts "srt://localhost:1937?mode=caller"
```

### No output to Kick

```bash
# Check health status
curl http://localhost:8088/health

# Verify pipeline is playing
# Look for: "pipeline_state": "playing"

# Check for encoding errors
docker compose logs srt-switcher | grep ERROR
```

### Dashboard not loading

```bash
# Check dashboard health
curl http://localhost:3000/api/health

# Verify controller is running
curl http://localhost:8089/health

# Check dashboard logs
docker compose logs stream-dashboard
```

## Development

### Rebuild specific service

```bash
docker compose build srt-switcher
docker compose up -d srt-switcher
```

### Test locally without Docker

```bash
cd srt-switcher
SRT_PORT=9000 \
FALLBACK_VIDEO=/path/to/video.mp4 \
KICK_URL=rtmps://server/app \
KICK_KEY=key \
python3 switcher.py
```

## Architecture Benefits

Compared to the previous 10-container system:

1. **Simplicity**: 3 containers vs 10 containers
2. **Lower Latency**: Direct SRT → encode → Kick (no RTMP relay)
3. **Resource Efficiency**: One GStreamer process instead of multiple FFmpeg processes
4. **Easier Debugging**: All streaming logic in one place
5. **Instant Switching**: Hardware-level pad switching (no re-encoding delay)
6. **No Dependencies**: Self-contained, no nginx-rtmp needed

## Technical Details

See [`srt-switcher/README.md`](srt-switcher/README.md) for detailed implementation documentation.