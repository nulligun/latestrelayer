# RTMP Stream Relay System

A Docker-based RTMP relay system with two input streams and switchable output to Kick streaming platform.

## Architecture

```
┌─────────────────┐
│  Video Files    │
│ (Host System)   │
└────────┬────────┘
         │
    ┌────┴─────┐
    │          │
┌───▼───┐  ┌──▼────┐
│offline│  │offline│
│  .mp4 │  │ 2.mp4 │
└───┬───┘  └───┬───┘
    │          │
    │      ┌───▼────────────┐
    │      │ ffmpeg-dev-cam │
    │      │  (dev only)    │
    │      └───┬────────────┘
    │          │
┌───▼──────────▼────┐
│  ffmpeg-offline   │
└───┬───────────────┘
    │
    │      ┌────────────────┐
    └──────► NGINX-RTMP     │◄──┐
           │   Server       │   │
           │ Port 1936      │   │
           └───┬────────────┘   │
               │                │
        ┌──────▼────────┐       │
        │ Stream        │       │
        │ Switcher      │───────┘
        │ (GStreamer)   │
        │ API: 8088     │
        └──────┬────────┘
               │
        ┌──────▼────────┐
        │ ffmpeg-kick   │◄────────┐
        │   Pusher      │         │
        └──────┬────────┘         │
               │          ┌───────┴────────┐
        ┌──────▼────────┐ │    Stream      │
        │  Kick Server  │ │  Controller    │
        └───────────────┘ │  (Docker API)  │
                          │  API: 8089     │
                          └────────────────┘
```

## Services

### 1. nginx-rtmp
- **Purpose**: Central RTMP relay hub
- **Ports**: 1936 (RTMP), 8080 (HTTP stats)
- **Streams**: 
  - `rtmp://nginx-rtmp:1936/live/offline` - Offline video loop
  - `rtmp://nginx-rtmp:1936/live/cam` - Camera feed (dev mode)
  - `rtmp://nginx-rtmp:1936/live/program` - Output stream

### 2. ffmpeg-offline
- **Purpose**: Continuously streams offline.mp4
- **Output**: `rtmp://nginx-rtmp:1936/live/offline`
- **Profiles**: All (default and dev)

### 3. ffmpeg-dev-cam
- **Purpose**: Simulates camera feed using offline2.mp4
- **Output**: `rtmp://nginx-rtmp:1936/live/cam`
- **Profiles**: dev only

### 4. stream-switcher
- **Purpose**: Switches between input streams using GStreamer
- **API Port**: 8088
- **Output**: `rtmp://nginx-rtmp:1936/live/program`
- **Default**: Starts with offline stream

### 5. ffmpeg-kick
- **Purpose**: Relays program stream to Kick
- **Output**: Kick RTMPS server

### 6. stream-auto-switcher
- **Purpose**: Automatically switches between camera and offline streams based on availability and quality
- **Monitors**: nginx-rtmp stats for stream presence and bitrate
- **Profiles**: auto (optional, can be run manually or omitted)
- **Key Features**:
  - XML-based parsing of nginx-rtmp statistics
  - Configurable minimum bitrate threshold (default: 300 kbps)
  - Grace period for stream stability before switching
  - Automatic fallback to offline when camera quality degrades

### 7. stream-controller
- **Purpose**: Container lifecycle management via REST API
- **API Port**: 8089
- **Capabilities**: Start/stop/restart/status for all containers

### 8. stream-dashboard
- **Purpose**: Web UI for monitoring and controlling the streaming system
- **Port**: 3000 (HTTP/WebSocket)
- **Features**:
  - Real-time system metrics (CPU, memory, load)
  - RTMP bandwidth monitoring
  - Current scene display
  - Container start/stop/restart controls
  - Live updates via WebSocket every 2 seconds
- **Technology**: Vue.js 3 + Express + WebSocket

## Prerequisites

- Docker and Docker Compose
- Video files:
  - `/home/mulligan/offline.mp4` - Main offline video
  - `/home/mulligan/offline2.mp4` - Dev camera simulation

## Configuration

### Single Instance Setup

Edit [`.env`](.env:1) file:

```bash
# Instance Identification
COMPOSE_PROJECT_NAME=relayer

# Kick Configuration
KICK_URL=rtmps://fa723fc1b171.global-contribute.live-video.net/app
KICK_KEY=sk_us-west-2_xXX3uY9mOSJP_eZTBNAACIwJSKv3EMu6Dhh2La1XZ1s

# Exposed Ports (change these for multiple instances)
RTMP_PORT=1936
HTTP_STATS_PORT=8080
SWITCHER_API_PORT=8088
CONTROLLER_API_PORT=8089
DASHBOARD_PORT=3000

# Video Files
OFFLINE_VIDEO_PATH=/home/mulligan/offline.mp4
OFFLINE_VIDEO_PATH_2=/home/mulligan/offline2.mp4
```

### Multi-Instance Setup

To run multiple instances on the same server, each instance needs unique ports. Example configurations are provided:

- [`.env.instance1`](.env.instance1:1) - Instance 1 (ports: 1936, 8080, 8088)
- [`.env.instance2`](.env.instance2:1) - Instance 2 (ports: 2036, 8180, 8188)
- [`.env.instance3`](.env.instance3:1) - Instance 3 (ports: 2136, 8280, 8288)

**Port Mapping:**

| Instance | RTMP Port | HTTP Stats | Switcher API | Controller API | Dashboard |
|----------|-----------|------------|--------------|----------------|-----------|
| 1        | 1936      | 8080       | 8088         | 8089           | 3000      |
| 2        | 2036      | 8180       | 8188         | 8189           | 3100      |
| 3        | 2136      | 8280       | 8288         | 8289           | 3200      |

**Important:** Each instance must have a unique `COMPOSE_PROJECT_NAME` to avoid container name conflicts.

## Usage

### Start Services

**Production Mode** (without dev camera):
```bash
docker compose up -d
```

**Development Mode** (with simulated camera):
```bash
docker compose --profile dev up -d
```

**With Auto-Switcher** (automatically manages stream switching):
```bash
docker compose --profile auto up -d
```

**Dev Mode + Auto-Switcher** (simulated camera with automatic switching):
```bash
docker compose --profile dev --profile auto up -d
```

### Switch Between Streams

#### Manual Switching

The stream switcher exposes an HTTP API on port 8088:

**Switch to offline stream:**
```bash
curl "http://localhost:8088/switch?src=offline"
```

**Switch to camera stream:**
```bash
curl "http://localhost:8088/switch?src=cam"
```

**Health check:**
```bash
curl "http://localhost:8088/health"
```

#### Automatic Switching

When running with the `auto` profile, the auto-switcher service automatically manages stream switching based on:

- **Stream Availability**: Detects when camera stream starts/stops
- **Bitrate Quality**: Monitors video bitrate and switches away if below threshold
- **Stability Period**: Waits for camera to be stable before switching to it
- **Grace Period**: Allows temporary quality degradation before switching away

**Configuration** (via `.env` file):
```bash
# Minimum bitrate threshold in kbps (default: 300)
MIN_BITRATE_KBPS=300

# How often to poll nginx stats in seconds (default: 0.5)
AUTO_SWITCHER_POLL_SECS=0.5

# Seconds to wait before switching away from degraded camera (default: 3.0)
AUTO_SWITCHER_CAM_MISS_TIMEOUT=3.0

# Seconds camera must be stable before switching to it (default: 2.0)
AUTO_SWITCHER_CAM_BACK_STABILITY=2.0
```

**View auto-switcher logs:**
```bash
docker compose logs -f stream-auto-switcher
```

### Web Dashboard

Access the web dashboard for a visual interface:

```bash
# Open in browser
http://localhost:3000
```

The dashboard provides:
- Real-time system metrics (CPU, memory, load)
- RTMP stream statistics and bandwidth
- Current active scene indicator
- Visual container control (start/stop/restart buttons)
- Live updates every 2 seconds via WebSocket

### Container Control API

The stream controller exposes an HTTP API on port 8089 for managing container lifecycle:

**Start the kick stream:**
```bash
curl -X POST "http://localhost:8089/container/ffmpeg-kick/start"
```

**Stop the kick stream:**
```bash
curl -X POST "http://localhost:8089/container/ffmpeg-kick/stop"
```

**Restart the kick stream:**
```bash
curl -X POST "http://localhost:8089/container/ffmpeg-kick/restart"
```

**Check kick stream status:**
```bash
curl "http://localhost:8089/container/ffmpeg-kick/status"
```

**List all containers:**
```bash
curl "http://localhost:8089/containers"
```

**Health check:**
```bash
curl "http://localhost:8089/health"
```

**Control other containers:**
```bash
# Stop offline stream
curl -X POST "http://localhost:8089/container/ffmpeg-offline/stop"

# Start dev camera stream
curl -X POST "http://localhost:8089/container/ffmpeg-dev-cam/start"

# Check stream switcher status
curl "http://localhost:8089/container/stream-switcher/status"
```

### Monitor Services

**View logs:**
```bash
# All services
docker compose logs -f

# Specific service
docker compose logs -f stream-switcher
docker compose logs -f ffmpeg-kick
```

**NGINX RTMP stats:**
```bash
curl http://localhost:8080/stat
# Or open in browser: http://localhost:8080/stat
```

**Health check all services:**
```bash
curl http://localhost:8080/health  # nginx-rtmp
curl http://localhost:8088/health  # stream-switcher
curl http://localhost:8089/health  # stream-controller
curl http://localhost:3000/api/health  # stream-dashboard
```

**Web Dashboard:**
```bash
# Open in browser
http://localhost:3000
```

### Stop Services

```bash
docker compose down
```

## Running Multiple Instances

### Start Multiple Instances Simultaneously

**Instance 1 (default ports):**
```bash
docker compose --env-file .env.instance1 up -d
```

**Instance 2 (offset ports):**
```bash
docker compose --env-file .env.instance2 up -d
```

**Instance 3 (offset ports):**
```bash
docker compose --env-file .env.instance3 up -d
```

### Access Different Instances

Each instance exposes its services on different ports:

**Instance 1:**
```bash
# Stream switching
curl "http://localhost:8088/switch?src=offline"

# RTMP stream
rtmp://localhost:1936/live/program

# Stats
curl http://localhost:8080/stat
```

**Instance 2:**
```bash
# Stream switching
curl "http://localhost:8188/switch?src=offline"

# RTMP stream
rtmp://localhost:2036/live/program

# Stats
curl http://localhost:8180/stat
```

**Instance 3:**
```bash
# Stream switching
curl "http://localhost:8288/switch?src=offline"

# RTMP stream
rtmp://localhost:2136/live/program

# Stats
curl http://localhost:8280/stat
```

### Stop Specific Instance

```bash
# Stop instance 1
docker compose --env-file .env.instance1 down

# Stop instance 2
docker compose --env-file .env.instance2 down

# Stop instance 3
docker compose --env-file .env.instance3 down
```

### View Logs for Specific Instance

```bash
# Instance 1 logs
docker compose --env-file .env.instance1 logs -f

# Instance 2 logs
docker compose --env-file .env.instance2 logs -f
```

### Custom Port Configuration

To use custom ports, create your own `.env.custom` file:

```bash
# Copy an existing template
cp .env.instance1 .env.custom

# Edit ports to avoid conflicts
# RTMP_PORT=3036
# HTTP_STATS_PORT=8380
# SWITCHER_API_PORT=8388

# Start with custom config
docker compose --env-file .env.custom up -d
```

## Service Startup Order

1. **nginx-rtmp** - Starts first and waits for health check
2. **ffmpeg-offline** - Starts after nginx-rtmp is healthy
3. **ffmpeg-dev-cam** - Starts after nginx-rtmp is healthy (dev profile only)
4. **stream-switcher** - Starts after nginx-rtmp and ffmpeg-offline
5. **stream-auto-switcher** - Starts after stream-switcher is healthy (auto profile only)
6. **ffmpeg-kick** - Starts after stream-switcher is healthy
7. **stream-controller** - Starts independently (no dependencies)
8. **stream-dashboard** - Starts after stream-controller and nginx-rtmp are healthy

## Troubleshooting

### Stream not switching

**Manual switching:**
- Check if the desired input stream is running:
  ```bash
  docker compose logs ffmpeg-offline
  docker compose logs ffmpeg-dev-cam
  ```
- Verify stream-switcher is running:
  ```bash
  curl http://localhost:8088/health
  ```

**Automatic switching:**
- Check if auto-switcher is running:
  ```bash
  docker compose logs stream-auto-switcher
  ```
- Verify camera stream has sufficient bitrate:
  ```bash
  curl http://localhost:8080/stat | grep -A5 "cam"
  ```
- Check auto-switcher configuration in [`.env`](.env:1)
- Ensure `MIN_BITRATE_KBPS` threshold is appropriate for your stream

### No output to Kick
- Check if ffmpeg-kick is running:
  ```bash
  curl http://localhost:8089/container/ffmpeg-kick/status
  ```
- Check ffmpeg-kick logs:
  ```bash
  docker compose logs ffmpeg-kick
  ```
- Verify Kick credentials in [`.env`](.env:1)
- Ensure stream-switcher is outputting to program stream

### Container control issues
- Check if stream-controller is running:
  ```bash
  curl http://localhost:8089/health
  ```
- Verify Docker socket access:
  ```bash
  docker compose logs stream-controller
  ```
- Check container status:
  ```bash
  curl http://localhost:8089/containers
  ```

### Service won't start
- Check service health:
  ```bash
  docker compose ps
  ```
- View specific service logs:
  ```bash
  docker compose logs <service-name>
  ```
- Rebuild containers if needed:
  ```bash
  docker compose build --no-cache
  docker compose up -d
  ```

### Video files not found
- Verify file paths in [`.env`](.env:1) match actual file locations
- Ensure files are readable by Docker:
  ```bash
  ls -l /home/mulligan/offline.mp4
  ls -l /home/mulligan/offline2.mp4
  ```

## Development

### Rebuild specific service
```bash
docker compose build stream-switcher
docker compose up -d stream-switcher
```

### Test without Kick output
Comment out the `ffmpeg-kick` service in [`docker-compose.yml`](docker-compose.yml:1) and restart.

### View RTMP streams locally
Use VLC or ffplay to view streams:
```bash
ffplay rtmp://localhost:1936/live/offline
ffplay rtmp://localhost:1936/live/cam
ffplay rtmp://localhost:1936/live/program
```

## Technical Details

### Auto-Switcher Architecture

The auto-switcher monitors nginx-rtmp statistics and automatically controls the stream switcher:

**Monitoring Process:**
1. Polls nginx-rtmp `/stat` endpoint every 0.5 seconds
2. Parses XML response using `xml.etree.ElementTree`
3. Extracts stream presence, publishing status, and video bitrate
4. Compares bitrate against `MIN_BITRATE_KBPS` threshold

**State Machine:**
- **OFFLINE_ACTIVE**: Currently showing offline stream
- **WAITING_FOR_CAM**: Camera detected with good quality, waiting for stability
- **CAM_STABLE**: Camera stream is active and meeting quality requirements
- **CAM_UNSTABLE**: Camera exists but below bitrate threshold

**Decision Logic:**
- Switch TO camera: When stream is present, publishing, has sufficient bitrate, and stable for `CAM_BACK_STABILITY` seconds
- Switch FROM camera: When stream is missing, not publishing, or below bitrate threshold for `CAM_MISS_TIMEOUT` seconds

**Quality Monitoring:**
- Parses `<bw_video>` element from nginx-rtmp stats (bytes/sec)
- Converts to kbps: `(bytes_per_sec * 8) / 1000`
- Compares against configurable threshold (default: 300 kbps)
- Prevents switching to degraded camera feeds

### Stream Specifications
- **Resolution**: 1920x1080 (1080p)
- **Frame Rate**: 30 fps
- **Video Codec**: H.264 (x264)
- **Video Bitrate**: 3000 kbps
- **Audio Codec**: AAC
- **Audio Bitrate**: 128 kbps
- **Audio Sample Rate**: 48000 Hz
- **Audio Channels**: 2 (stereo)

### GStreamer Pipeline
The stream-switcher uses GStreamer's input-selector element for seamless switching:
- Decodes both input streams to raw video/audio
- Normalizes to 1080p30 format
- Uses input-selector for instant switching
- Re-encodes to H.264/AAC
- Outputs to FLV/RTMP

### Buffering Configuration

The system is optimized for **smooth playback** over ultra-low latency:

**NGINX RTMP Server:**
- `chunk_size 8192` - Doubled from default 4096 for better throughput and less overhead
- `buflen 10s` - 10 second server-side buffer to smooth out bursty input from GStreamer
- `max_message 10M` - Handles large video frames (especially keyframes) without dropping
- `wait_key on` - Waits for keyframe before starting playback (prevents partial GOP)
- `wait_video on` - Ensures video is present before starting playback
- `sync 10ms` - Allows 10ms audio/video sync tolerance
- **Expected latency**: 10-15 seconds total
- **Benefit**: Server-side buffering absorbs encoder variations and network hiccups

**FFmpeg Kick Pusher:**
- `-probesize 10M` - Analyzes 10MB of input for better stream detection
- `-analyzeduration 5000000` - Analyzes 5 seconds of input before starting
- `-rtmp_buffer 5000` - 5 second RTMP buffer (both input and output)
- `-v info -stats` - Verbose logging to monitor frame drops and performance
- No `-rtmp_live` flag - Removed to prevent aggressive frame dropping
- **Expected latency**: 2-5 seconds
- **Benefit**: Smooth, stutter-free playback on Kick with full diagnostic visibility

**GStreamer Queue Buffers:**
- Video queue: 30 buffers (10x increase from original 3)
- Audio queue: 30 buffers (10x increase from original 3)
- Leak policy: Upstream (drop new frames when full, keep recent complete frames)
- **Benefit**: Better handling of temporary network variations and downstream processing delays

These settings prioritize reliable, smooth streaming over minimal latency. The total latency is approximately 15-20 seconds end-to-end, which is acceptable for most live streaming scenarios. If you need lower latency, you can reduce the buffer sizes, but this may reintroduce stuttering under network stress.

### Network
All services run in a dedicated Docker bridge network [`rtmp-network`](docker-compose.yml:93) for internal communication.