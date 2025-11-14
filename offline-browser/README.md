# Offline Browser Container

Dedicated container for rendering web pages in a headless browser and streaming them to the compositor.

## Overview

This container uses Xvfb (X Virtual Framebuffer) to create a virtual display, launches Chromium to render a web page, and streams the display output (including audio) to the compositor's TCP server (port 1940). It includes automatic reconnection with exponential backoff and health checking to ensure reliable streaming.

## Features

- **Headless Browser Rendering**: Uses Xvfb + Chromium for headless web page rendering
- **Audio Support**: PulseAudio captures webpage audio (including autoplay)
- **Real-time Streaming**: FFmpeg x11grab captures display at 30fps with low latency
- **Auto-Reconnection**: Automatically reconnects if ffmpeg crashes or connection fails
- **Exponential Backoff**: Smart retry logic (1s → 2s → 4s → 8s → 10s max)
- **Health Checking**: Verifies all processes (Xvfb, Chromium, ffmpeg) are running
- **No Auto-Restart**: Container uses `restart: "no"` - internal script handles all retries
- **Docker Network**: Connects to compositor via internal docker network

## Configuration

The container is configured via environment variables in `.env`:

- `OFFLINE_SOURCE_URL`: The webpage URL to render (required)
- `COMPOSE_PROJECT_NAME`: Project name prefix for container name

Additional environment variables (with defaults):
- `COMPOSITOR_HOST=compositor` - Compositor hostname
- `COMPOSITOR_PORT=1940` - Compositor TCP port
- `DISPLAY_RESOLUTION=1920x1080` - Virtual display resolution
- `FRAME_RATE=30` - Capture frame rate
- `RETRY_DELAY=1` - Initial retry delay in seconds
- `MAX_RETRY_DELAY=10` - Maximum retry delay in seconds
- `DISPLAY=:99` - X display number

## Usage

### Start the Container

```bash
# Build and start
docker compose up -d offline-browser

# View logs
docker compose logs -f offline-browser
```

### Check Health Status

```bash
# Check container health
docker compose ps offline-browser

# Manual health check
docker compose exec offline-browser /app/healthcheck.sh
```

### Stop the Container

```bash
docker compose stop offline-browser
```

## How It Works

### Startup Sequence

1. Container waits for compositor to start (`depends_on: compositor`)
2. Xvfb starts on display `:99` at 1920x1080 resolution
3. PulseAudio daemon starts for audio capture
4. Chromium launches in kiosk mode rendering `OFFLINE_SOURCE_URL`
5. Wait 5 seconds for page to load (DOMContentLoaded)
6. ffmpeg begins capturing display and audio
7. Stream sent to `tcp://compositor:1940`
8. Health check monitors all processes

### Process Architecture

```
Xvfb :99 (1920x1080x24)
    ├─→ Chromium (kiosk mode)
    │   └─→ Renders webpage with audio
    └─→ FFmpeg x11grab
        ├─→ Captures display at 30fps
        ├─→ Captures PulseAudio output
        └─→ Encodes H.264 + AAC → TCP stream
```

### Reconnection Logic

When ffmpeg exits or crashes:

1. Script detects exit and logs the error
2. Waits with exponential backoff (1s, 2s, 4s, 8s, 10s max)
3. Attempts reconnection automatically
4. On successful connection, delay resets to 1s
5. Xvfb and Chromium remain running during reconnects

### Health Check

The health check script (`healthcheck.sh`) verifies:

- ✅ Xvfb process exists
- ✅ Chromium process exists and is rendering
- ✅ ffmpeg process exists
- ✅ ffmpeg is running (not zombie/defunct)
- ✅ ffmpeg is consuming CPU (actively encoding)

Returns:
- **Exit 0 (healthy)**: All processes streaming
- **Exit 1 (unhealthy)**: One or more processes not running

## File Structure

```
offline-browser/
├── Dockerfile           # Container definition
├── entrypoint.sh        # Main startup and streaming loop
├── healthcheck.sh       # Health verification script
└── README.md           # This file
```

## FFmpeg Command

The container uses the following ffmpeg command with timestamp synchronization and thread queue buffering:

```bash
ffmpeg \
  -thread_queue_size 512 \
  -f x11grab -video_size 1920x1080 -framerate 30 \
  -use_wallclock_as_timestamps 1 -i :99.0 \
  -thread_queue_size 512 \
  -f pulse -i default \
  -async 1 \
  -drop_pkts_on_overflow 0 \
  -r 30 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -vsync cfr \
  -f mpegts tcp://compositor:1940
```

**Threading Parameters:**
- `-thread_queue_size 512`: Increases input thread queue from default 8 to 512 packets (prevents blocking)
  - Applied to both x11grab and pulse inputs to prevent either from blocking
  - Eliminates "Thread message queue blocking" warnings
- `-drop_pkts_on_overflow 0`: Never drop packets on buffer overflow (wait for buffer space instead)
  - Prevents the `dup=XX drop=XX` packet loss issues
  - Maintains stream continuity at the cost of possible latency increase

**Video Parameters:**
- `-f x11grab`: Capture from X11 display
- `-video_size 1920x1080`: Capture resolution
- `-framerate 30`: 30 frames per second (input capture rate)
- `-use_wallclock_as_timestamps 1`: Use system clock for timestamps (prevents drift)
- `-i :99.0`: X display :99
- `-r 30`: Force output to exactly 30fps (duplicates frames for static content)
- `-preset veryfast`: Fast encoding
- `-tune zerolatency`: Low-latency tuning
- `-pix_fmt yuv420p`: Compatible pixel format
- `-vsync cfr`: Constant frame rate (prevents timestamp jitter)

**Audio Parameters:**
- `-f pulse`: Capture from PulseAudio
- `-i default`: Default audio sink
- `-async 1`: Audio sync with 1-sample correction threshold (handles clock drift)
- `-b:a 128k`: Audio bitrate
- `-ar 48000 -ac 2`: 48kHz stereo audio

**Synchronization Parameters:**
The synchronization flags prevent timestamp drift between x11grab and PulseAudio:
- `-use_wallclock_as_timestamps 1`: Forces x11grab to use system time instead of capture time
- `-async 1`: Enables audio sync to compensate for clock differences
- `-drop_pkts_on_overflow 0`: Prevents packet dropping which can cause connection issues
- `-r 30`: Forces constant 30fps output by duplicating frames when content is static
- `-vsync cfr`: Ensures constant frame rate output without timestamp jumps
- `-thread_queue_size 512`: Prevents capture threads from blocking when main thread is busy

**Why These Flags Matter:**
- **`-r 30`**: When displaying static web pages, x11grab won't capture new frames if nothing changes. This forces 30fps output by duplicating frames, keeping data flowing constantly.
- **`-drop_pkts_on_overflow 0`**: Prevents FFmpeg from dropping packets when buffers fill, which would cause gaps in the stream and trigger compositor disconnection.

**Compositor Integration:**
The compositor's video watchdog timeout is configured via `VIDEO_WATCHDOG_TIMEOUT` (default: 2.0s). This longer timeout (vs 0.2s for SRT) accounts for the 3-second buffer queues in the video pipeline, allowing natural buffer filling without false disconnections.

These parameters eliminate "Non-monotonous DTS", "Queue input is backward in time", and "Thread message queue blocking" errors, and prevent disconnections caused by static content or buffer fluctuations.

## Chromium Flags

The browser is launched with these critical flags:

- `--kiosk`: Fullscreen mode with no UI
- `--no-sandbox`: Required for running as root in containers
- `--disable-dev-shm-usage`: Reduces /dev/shm memory usage
- `--disable-gpu`: Disables GPU acceleration (not available in container)
- `--autoplay-policy=no-user-gesture-required`: Enables audio autoplay
- `--disable-notifications`: Prevents notification popups
- `--no-first-run`: Skips first-run setup

## Troubleshooting

### Container keeps showing unhealthy

```bash
# Check logs for errors
docker compose logs offline-browser

# Verify all processes are running
docker compose exec offline-browser ps aux

# Check X server
docker compose exec offline-browser echo $DISPLAY
```

### Chromium fails to start

```bash
# Check if URL is accessible
docker compose exec offline-browser curl -I ${OFFLINE_SOURCE_URL}

# Verify X server is running
docker compose exec offline-browser pgrep Xvfb
```

### ffmpeg fails to connect

```bash
# Ensure compositor is running
docker compose ps compositor

# Check compositor logs
docker compose logs compositor

# Verify network connectivity
docker compose exec offline-browser nc -zv compositor 1940
```

### No audio in stream

```bash
# Check PulseAudio status
docker compose exec offline-browser pulseaudio --check

# List audio devices
docker compose exec offline-browser pactl list sinks

# Verify webpage has audio
# (Check browser console logs in entrypoint.sh output)
```

### Page not rendering correctly

```bash
# Increase page load wait time in entrypoint.sh
# Change: sleep 5
# To: sleep 10

# Check Chromium logs
docker compose logs offline-browser | grep -i chromium
```

## Integration with Compositor

The offline-browser container integrates with the compositor's video fallback feature:

1. Compositor must have `FALLBACK_SOURCE=video` in `.env`
2. Compositor listens on TCP port 1940
3. This container connects and streams browser capture
4. Compositor fades between black → browser → SRT as sources connect/disconnect

See [`compositor/README.md`](../compositor/README.md) for more details on the compositor's video fallback feature.

## Best Practices

1. **URL Selection**: Ensure the webpage is optimized for streaming (avoid heavy animations)
2. **Resolution**: Match compositor resolution (1920x1080 @ 30fps) for best quality
3. **Audio**: Test audio playback in the target webpage before deploying
4. **Performance**: Monitor CPU usage - expect 300-500% with all processes
5. **Memory**: Chromium can use 500MB-1GB - ensure sufficient container memory

## Performance Characteristics

- **CPU Usage**: 300-500% (Xvfb ~20%, Chromium ~150-300%, FFmpeg ~100-180%)
- **Memory**: 800MB-1.2GB (mostly Chromium)
- **Network**: ~3-5 Mbps output (H.264 @ veryfast preset)
- **Latency**: ~2-3 seconds end-to-end (page render + encode + network)

## Notes

- Container will never auto-restart on failure (internal script handles all retries)
- Exponential backoff prevents overwhelming the compositor during extended outages
- Health checks ensure monitoring systems can detect streaming issues
- All reconnection logic is logged for debugging
- Xvfb and Chromium remain running across FFmpeg reconnections for efficiency
- PulseAudio daemon persists to maintain audio capture capability