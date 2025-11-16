# Offline Image Container

Dedicated container for streaming a static image as video to the compositor.

## Overview

This container continuously streams a static image (PNG, JPG, etc.) as a looping video with silent audio to the compositor's TCP server (port 1940). It includes automatic reconnection with exponential backoff and health checking to ensure reliable video fallback.

## Features

- **Image-to-Video Streaming**: Converts static image into continuous video stream
- **Fixed Frame Rate**: Maintains constant 30fps output to prevent watchdog disconnections
- **Silent Audio Generation**: Generates silent stereo audio track for compositor compatibility
- **Auto-Reconnection**: Automatically reconnects if ffmpeg crashes or connection fails
- **Exponential Backoff**: Smart retry logic (1s → 2s → 4s → 8s → 10s max)
- **Health Checking**: Verifies ffmpeg process is running and actively streaming
- **No Auto-Restart**: Container uses `restart: "no"` - internal script handles all retries
- **Docker Network**: Connects to compositor via internal docker network

## Configuration

The container is configured via environment variables in `.env`:

- `BRB_IMAGE_PATH`: Path to offline image file on host (e.g., `/home/mulligan/offline.png`)
- `COMPOSE_PROJECT_NAME`: Project name prefix for container name

Additional environment variables (with defaults):
- `IMAGE_PATH=/image/offline.png` - Path inside container
- `COMPOSITOR_HOST=compositor` - Compositor hostname
- `COMPOSITOR_PORT=1940` - Compositor TCP port
- `RETRY_DELAY=1` - Initial retry delay in seconds
- `MAX_RETRY_DELAY=10` - Maximum retry delay in seconds

## Usage

### Start the Container

```bash
# Build and start
docker compose up -d offline-image

# View logs
docker compose logs -f offline-image
```

### Check Health Status

```bash
# Check container health
docker compose ps offline-image

# Manual health check
docker compose exec offline-image /app/healthcheck.sh
```

### Stop the Container

```bash
docker compose stop offline-image
```

## How It Works

### Startup Sequence

1. Container waits for compositor to start (`depends_on: compositor`)
2. Entrypoint script begins infinite reconnection loop
3. ffmpeg converts image to video stream with silent audio
4. Streams to `tcp://compositor:1940` at constant 30fps
5. Health check monitors ffmpeg process

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
offline-image/
├── Dockerfile           # Container definition
├── entrypoint.sh        # Main streaming loop with reconnection
├── healthcheck.sh       # Health verification script
└── README.md           # This file
```

## FFmpeg Command

The container uses the following ffmpeg command:

```bash
ffmpeg \
  -re \
  -loop 1 -framerate 30 -i /image/offline.png \
  -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 \
  -vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2" \
  -r 30 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts tcp://compositor:1940
```

**Real-Time Streaming:**
- `-re`: Read input at native frame rate (critical for preventing TCP buffer overflow)
  - Without this flag, ffmpeg processes and sends frames as fast as possible
  - Causes "Broken pipe" errors when TCP connection buffer overflows
  - Ensures smooth 30fps streaming instead of burst transmission

**Image Input Parameters:**
- `-loop 1`: Loop the image infinitely
- `-framerate 30`: Input frame rate (read image at 30fps)
- `-i /image/offline.png`: Input image file

**Audio Generation Parameters:**
- `-f lavfi`: Use libavfilter virtual device
- `-i anullsrc=channel_layout=stereo:sample_rate=48000`: Generate silent stereo audio at 48kHz

**Video Filter Parameters:**
- `-vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2"`: Video filter chain
  - `scale=1920:1080:force_original_aspect_ratio=decrease`: Scale image to fit within 1920x1080 while maintaining aspect ratio
  - `pad=1920:1080:(ow-iw)/2:(oh-ih)/2`: Pad scaled image to exactly 1920x1080 with black bars, centered

**Output Parameters:**
- `-r 30`: Force output frame rate to exactly 30fps (critical for watchdog)
- `-c:v libx264`: H.264 video encoding
- `-preset veryfast`: Fast encoding preset
- `-tune zerolatency`: Low-latency tuning
- `-pix_fmt yuv420p`: Compatible pixel format
- `-c:a aac`: AAC audio encoding
- `-b:a 128k`: Audio bitrate
- `-ar 48000 -ac 2`: 48kHz stereo audio
- `-f mpegts`: MPEG-TS container format

**Why Real-Time Streaming and Fixed Frame Rate Matter:**

The `-re`, `-r 30`, and `-framerate 30` flags work together to ensure reliable streaming:

1. **`-re`**: Controls transmission rate to prevent overwhelming the TCP connection
   - Static images can be encoded infinitely fast, causing buffer overflow
   - Matches the real-time streaming behavior of the offline-video container
   - Prevents "Broken pipe" errors from rapid data transmission

2. **`-framerate 30`**: Tells ffmpeg to read the looped image at 30fps
   - Sets the input frame rate for the static image

3. **`-r 30`**: Forces constant 30fps output by duplicating frames
   - Prevents irregular streams that trigger compositor watchdog timeout
   - Essential for static content that doesn't naturally have a frame rate

Without the `-re` flag, ffmpeg will generate frames as fast as possible, overwhelming the TCP buffer and causing connection failures. This matches the approach used in the offline-video and offline-browser containers.

## Troubleshooting

### Container keeps showing unhealthy

```bash
# Check logs for ffmpeg errors
docker compose logs offline-image

# Verify image file is accessible
docker compose exec offline-image ls -la /image/

# Check if compositor is reachable
docker compose exec offline-image ping compositor
```

### ffmpeg fails to connect

```bash
# Ensure compositor is running
docker compose ps compositor

# Check compositor logs
docker compose logs compositor

# Verify network connectivity
docker compose exec offline-image nc -zv compositor 1940
```

### Image file not found

```bash
# Verify BRB_IMAGE_PATH in .env exists
ls -la /path/to/your/image.png

# Check volume mount
docker compose exec offline-image ls -la /image/offline.png
```

### View real-time reconnection attempts

```bash
# Follow logs to see reconnection behavior
docker compose logs -f offline-image
```

## Integration with Compositor

The offline-image container integrates with the compositor's video fallback feature:

1. Compositor must have `FALLBACK_SOURCE=video` in `.env`
2. Compositor listens on TCP port 1940
3. This container connects and streams image as video
4. Compositor fades between black → image → SRT as sources connect/disconnect

See [`compositor/README.md`](../compositor/README.md) for more details on the compositor's video fallback feature.

## Best Practices

1. **Image Format**: Use PNG or JPG for best compatibility
2. **Resolution**: Any resolution supported - automatically scaled to 1920x1080
3. **File Size**: Keep images reasonably sized (< 5MB)
4. **Aspect Ratio**: Any aspect ratio supported - automatically fitted with black bars to 16:9 (1920x1080)
5. **Testing**: Test image display before deploying

The video filter chain automatically handles resolution and aspect ratio conversion:
- Images are scaled to fit within 1920x1080 while maintaining their original aspect ratio
- Black bars (letterbox/pillarbox) are added to center the image in the exact 1920x1080 frame
- Output is always exactly 1920x1080 @ 30fps regardless of input image dimensions

## Use Cases

- **"Be Right Back" screens**: Static message during stream interruptions
- **Maintenance announcements**: Display scheduled downtime information
- **Branding**: Show logo or channel art during offline periods
- **Technical difficulties**: Generic error message screen
- **Event countdowns**: Static countdown image before live events

## Notes

- Container will never auto-restart on failure (internal script handles all retries)
- Exponential backoff prevents overwhelming the compositor during extended outages
- Health checks ensure monitoring systems can detect streaming issues
- All reconnection logic is logged for debugging
- Audio track is required even though it's silent (compositor expects both video and audio)