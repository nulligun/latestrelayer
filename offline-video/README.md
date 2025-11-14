# Offline Video Container

Dedicated container for streaming offline/fallback video to the compositor.

## Overview

This container continuously streams a looping video file to the compositor's TCP server (port 1940). It includes automatic reconnection with exponential backoff and health checking to ensure reliable video fallback.

## Features

- **Infinite Loop Streaming**: Continuously streams video with `-stream_loop -1`
- **Auto-Reconnection**: Automatically reconnects if ffmpeg crashes or connection fails
- **Exponential Backoff**: Smart retry logic (1s → 2s → 4s → 8s → 10s max)
- **Health Checking**: Verifies ffmpeg process is running and actively streaming
- **No Auto-Restart**: Container uses `restart: "no"` - internal script handles all retries
- **Docker Network**: Connects to compositor via internal docker network

## Configuration

The container is configured via environment variables in `.env`:

- `BRB_VIDEO_PATH`: Path to offline video file on host (default: `/home/mulligan/offline.mp4`)
- `COMPOSE_PROJECT_NAME`: Project name prefix for container name

Additional environment variables (with defaults):
- `VIDEO_PATH=/video/offline.mp4` - Path inside container
- `COMPOSITOR_HOST=compositor` - Compositor hostname
- `COMPOSITOR_PORT=1940` - Compositor TCP port
- `RETRY_DELAY=1` - Initial retry delay in seconds
- `MAX_RETRY_DELAY=10` - Maximum retry delay in seconds

## Usage

### Start the Container

```bash
# Build and start
docker compose up -d offline-video

# View logs
docker compose logs -f offline-video
```

### Check Health Status

```bash
# Check container health
docker compose ps offline-video

# Manual health check
docker compose exec offline-video /app/healthcheck.sh
```

### Stop the Container

```bash
docker compose stop offline-video
```

## How It Works

### Startup Sequence

1. Container waits for compositor to start (`depends_on: compositor`)
2. Entrypoint script begins infinite reconnection loop
3. ffmpeg streams video to `tcp://compositor:1940`
4. Health check monitors ffmpeg process

### Reconnection Logic

When ffmpeg exits or crashes:

1. Script detects exit and logs the error
2. Waits with exponential backoff (1s, 2s, 4s, 8s, 10s max)
3. Attempts reconnection automatically
4. On successful connection, delay resets to 1s

### Health Check

The health check script (`healthcheck.sh`) verifies:

- ✅ ffmpeg process exists
- ✅ Process is running (not zombie/defunct)
- ✅ Process is consuming CPU (actively encoding)

Returns:
- **Exit 0 (healthy)**: ffmpeg is streaming
- **Exit 1 (unhealthy)**: ffmpeg not running or crashed

## File Structure

```
offline-video/
├── Dockerfile           # Container definition
├── entrypoint.sh        # Main streaming loop with reconnection
├── healthcheck.sh       # Health verification script
└── README.md           # This file
```

## FFmpeg Command

The container uses the following ffmpeg command:

```bash
ffmpeg \
  -re -stream_loop -1 \
  -i /video/offline.mp4 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts tcp://compositor:1940
```

**Parameters:**
- `-re`: Real-time streaming
- `-stream_loop -1`: Infinite loop
- `-preset veryfast`: Fast encoding
- `-tune zerolatency`: Low-latency tuning
- `-pix_fmt yuv420p`: Compatible pixel format
- `-b:a 128k`: Audio bitrate
- `-ar 48000 -ac 2`: 48kHz stereo audio

## Troubleshooting

### Container keeps showing unhealthy

```bash
# Check logs for ffmpeg errors
docker compose logs offline-video

# Verify video file is accessible
docker compose exec offline-video ls -la /video/

# Check if compositor is reachable
docker compose exec offline-video ping compositor
```

### ffmpeg fails to connect

```bash
# Ensure compositor is running
docker compose ps compositor

# Check compositor logs
docker compose logs compositor

# Verify network connectivity
docker compose exec offline-video nc -zv compositor 1940
```

### Video file not found

```bash
# Verify BRB_VIDEO_PATH in .env exists
ls -la /home/mulligan/offline.mp4

# Check volume mount
docker compose exec offline-video ls -la /video/offline.mp4
```

### View real-time reconnection attempts

```bash
# Follow logs to see reconnection behavior
docker compose logs -f offline-video
```

## Integration with Compositor

The offline-video container integrates with the compositor's video fallback feature:

1. Compositor must have `FALLBACK_SOURCE=video` in `.env`
2. Compositor listens on TCP port 1940
3. This container connects and streams video
4. Compositor fades between black → video → SRT as sources connect/disconnect

See [`compositor/README.md`](../compositor/README.md) for more details on the compositor's video fallback feature.

## Best Practices

1. **Video Format**: Use H.264/AAC in MP4 container for best compatibility
2. **Resolution**: Match compositor resolution (1920x1080 @ 30fps)
3. **File Size**: Smaller files loop more smoothly
4. **Audio**: Ensure audio is present and properly encoded
5. **Testing**: Test video playback before deploying

## Notes

- Container will never auto-restart on failure (internal script handles all retries)
- Exponential backoff prevents overwhelming the compositor during extended outages
- Health checks ensure monitoring systems can detect streaming issues
- All reconnection logic is logged for debugging