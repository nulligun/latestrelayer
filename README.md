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
        │ ffmpeg-kick   │
        │   Pusher      │
        └──────┬────────┘
               │
        ┌──────▼────────┐
        │  Kick Server  │
        └───────────────┘
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

## Prerequisites

- Docker and Docker Compose
- Video files:
  - `/home/mulligan/offline.mp4` - Main offline video
  - `/home/mulligan/offline2.mp4` - Dev camera simulation

## Configuration

Edit [`.env`](.env:1) file:

```bash
# Instance Identification
COMPOSE_PROJECT_NAME=relayer

# Kick Configuration
KICK_URL=rtmps://fa723fc1b171.global-contribute.live-video.net/app
KICK_KEY=sk_us-west-2_xXX3uY9mOSJP_eZTBNAACIwJSKv3EMu6Dhh2La1XZ1s

# Video Files
OFFLINE_VIDEO_PATH=/home/mulligan/offline.mp4
OFFLINE_VIDEO_PATH_2=/home/mulligan/offline2.mp4

# Ports
RTMP_PORT=1936
HTTP_STATS_PORT=8080
SWITCHER_API_PORT=8088
```

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

### Switch Between Streams

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
```

### Stop Services

```bash
docker compose down
```

## Service Startup Order

1. **nginx-rtmp** - Starts first and waits for health check
2. **ffmpeg-offline** - Starts after nginx-rtmp is healthy
3. **ffmpeg-dev-cam** - Starts after nginx-rtmp is healthy (dev profile only)
4. **stream-switcher** - Starts after nginx-rtmp and ffmpeg-offline
5. **ffmpeg-kick** - Starts after stream-switcher is healthy

## Troubleshooting

### Stream not switching
- Check if the desired input stream is running:
  ```bash
  docker compose logs ffmpeg-offline
  docker compose logs ffmpeg-dev-cam
  ```
- Verify stream-switcher is running:
  ```bash
  curl http://localhost:8088/health
  ```

### No output to Kick
- Check ffmpeg-kick logs:
  ```bash
  docker compose logs ffmpeg-kick
  ```
- Verify Kick credentials in [`.env`](.env:1)
- Ensure stream-switcher is outputting to program stream

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

### Network
All services run in a dedicated Docker bridge network [`rtmp-network`](docker-compose.yml:93) for internal communication.