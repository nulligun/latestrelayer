# Compositor Container

GStreamer-based video compositor with hybrid architecture for reliable streaming.

**Version: 2.1.0**

## Features

- **Immediate Startup**: Pipeline starts instantly with black screen + silence output
- **Video Fallback** (Optional): TCP server for looping video fallback (replaces black screen)
- **SRT Input**: Listens on port 1937 for incoming SRT streams
- **Automatic Fade Control**:
  - Fades in (1 second) when video/SRT stream connects
  - Fades out (1 second) after 200ms of no data
  - Returns to appropriate fallback when input disconnects
  - SRT always takes priority over video fallback
- **Hybrid Architecture**: Separate fallback, video, and SRT pipelines sharing single output
- **TCP Output**: Always streaming to TCP port 5000 (even before any input connects)
- **Resolution**: 1920x1080 @ 30fps

## Quick Start

### 1. Build the Container

```bash
docker compose build compositor
```

### 2. Start the Container

```bash
docker compose up -d compositor
```

### 3. Check Logs

```bash
docker compose logs -f compositor
```

You should see:
```
Pipeline started. VLC: tcp://127.0.0.1:5000
SRT listening on port 1937
```

## Testing

### Send Test Stream with FFmpeg

**Basic test with a video file:**

```bash
ffmpeg -re -i /path/to/video.mp4 \
  -c:v libx264 -preset ultrafast -tune zerolatency \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:1937?mode=caller&latency=2000"
```

**With specific resolution (recommended):**

```bash
ffmpeg -re -i /path/to/video.mp4 \
  -vf scale=1920:1080 \
  -c:v libx264 -preset ultrafast -tune zerolatency -b:v 3000k \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:1937?mode=caller&latency=2000"
```

**Loop a test pattern:**

```bash
ffmpeg -re -f lavfi -i testsrc=size=1920x1080:rate=30 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -c:v libx264 -preset ultrafast -tune zerolatency -b:v 3000k \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:1937?mode=caller&latency=2000"
```

### View Output with VLC

**Command line:**

```bash
vlc tcp://localhost:5000
```

**VLC GUI:**
1. Open VLC
2. Media → Open Network Stream
3. Enter: `tcp://localhost:5000`
4. Click Play

## Expected Behavior

1. **Container starts**:
   - ✅ **Immediately starts streaming** black screen + silence to TCP port 5000
   - Pipeline is fully operational before SRT connection
   - Downstream consumers can connect and receive valid stream instantly

2. **SRT connects**:
   - Video/audio fade in over 1 second
   - SRT feed composites over the black fallback

3. **SRT active**:
   - Video plays normally at full opacity
   - Audio at full volume

4. **SRT disconnects**:
   - After 1 second of no data, video/audio fade out over 1 second
   - Returns to black screen + silence fallback
   - **Output stream never stops** - seamless transition

5. **SRT reconnects**:
   - Automatically detects reconnection
   - Fades back in over 1 second

## State Machine

### Without Video Fallback (FALLBACK_SOURCE not set)
```
FALLBACK_ONLY (black) → SRT_TRANSITIONING (fade 1s) → SRT_CONNECTED
     ↑                                                        ↓
     └────────────── SRT disconnects (fade 1s) ──────────────┘
```

### With Video Fallback (FALLBACK_SOURCE=video)
```
FALLBACK_ONLY (black)
     ↓ TCP connects
VIDEO_TRANSITIONING (fade 1s)
     ↓
VIDEO_CONNECTED (video loop)
     ↓ SRT connects              ↓ TCP disconnects
SRT_TRANSITIONING  ←→  VIDEO_TRANSITIONING
     ↓                            ↓
SRT_CONNECTED              FALLBACK_ONLY
     ↓ SRT disconnects
VIDEO_TRANSITIONING (if video still connected)
  or
FALLBACK_ONLY (if video disconnected)
```

**Key Improvement in v2.1.0**: Optional video fallback layer between black screen and SRT feed, allowing for a "Be Right Back" video instead of black screen.

## Architecture

### Hybrid Pipeline Design (v2.1.0)

The compositor uses a **hybrid architecture** with independent stages:

```
┌─────────────────────────────────────────────────────────────┐
│ FALLBACK SOURCES (Always Running)                          │
│   videotestsrc (black) ──→ videoconvert ──→ compositor     │
│   audiotestsrc (silence) ─→ audioconvert ─→ audiomixer     │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ VIDEO ELEMENTS (Optional - when FALLBACK_SOURCE=video)      │
│   tcpserversrc:1940 → decodebin ─┬→ queue → alpha ─────┐   │
│                                   └→ queue → volume ────┤   │
└───────────────────────────────────────────────────────────┼─┘
                                                            │
┌─────────────────────────────────────────────────────────┐ │
│ SRT ELEMENTS (Dynamic - Added on startup)               │ │
│   srtserversrc:1937 → decodebin ─┬→ queue → alpha ─────┤ │
│                                   └→ queue → volume ────┤ │
└─────────────────────────────────────────────────────────┼─┤
                                                            │ │
┌───────────────────────────────────────────────────────────┼─┤
│ SHARED OUTPUT (Always Running)                           │ │
│   compositor ←──────────────────────────────────────────┴─┘
│       ↓                                                    │
│   x264enc → mpegtsmux → tcpserversink:5000                │
│       ↑                                                    │
│   audiomixer ←────────────────────────────────────────────┘
└──────────────────────────────────────────────────────────────┘
```

### Key Components

**Fallback Stage** (always present):
- **videotestsrc**: Black screen (1920x1080@30fps)
- **audiotestsrc**: Silence (48kHz stereo)
- Always linked to output, ensures stream never stops

**Video Stage** (optional - enabled with FALLBACK_SOURCE=video):
- **tcpserversrc**: TCP server listening on port 1940
- **decodebin**: Automatically decodes incoming MPEG-TS stream
- **alpha**: Controls video transparency (0.0 = invisible, 1.0 = opaque)
- **volume**: Controls audio level (0.0 = muted, 1.0 = full)
- Can be removed/re-added to reset TCP server after disconnect

**SRT Stage** (dynamically managed):
- **srtserversrc**: Listens for SRT on port 1937
- **decodebin**: Automatically decodes incoming streams
- **alpha**: Controls video transparency (0.0 = invisible, 1.0 = opaque)
- **volume**: Controls audio level (0.0 = muted, 1.0 = full)
- Takes priority over video fallback when connected
- Can be removed/re-added without affecting output

**Output Stage** (always running):
- **compositor**: Blends layers (black → video → SRT)
- **audiomixer**: Mixes audio from all sources
- **x264enc**: H.264 encoding with low-latency settings
- **mpegtsmux**: MPEG-TS container
- **tcpserversink**: Always streaming on port 5000

### Benefits of Hybrid Architecture

✅ **Immediate startup** - No waiting for any connection
✅ **Reliable output** - Stream never stops, even during source issues
✅ **Clean isolation** - Problems in one stage don't affect others
✅ **Easy debugging** - Each stage can be inspected independently
✅ **Graceful transitions** - Fade in/out between layers
✅ **Flexible fallback** - Choose between black screen or looping video

## Troubleshooting

### Container won't start

```bash
# Check logs
docker compose logs compositor

# Rebuild
docker compose build --no-cache compositor
docker compose up -d compositor
```

### Can't connect with FFmpeg

```bash
# Verify port is exposed
docker compose ps compositor

# Check if SRT port is listening
netstat -uln | grep 1937

# Try with explicit host
ffmpeg ... "srt://127.0.0.1:1937?mode=caller"
```

### VLC shows black screen

**This is normal behavior!** The compositor always starts with a black screen:
- Container outputs black screen + silence immediately on startup
- This is the **fallback stream** - always available
- Send an SRT stream to see content fade in over the black screen
- If you see a black screen, it means the output is working correctly
- To verify: check that VLC is receiving data (buffering/playing)

### VLC can't connect

```bash
# Verify TCP port is exposed
docker compose ps compositor

# Check if port is listening
netstat -tln | grep 5000

# Try localhost explicitly
vlc tcp://127.0.0.1:5000
```

### Pipeline errors

```bash
# Check GStreamer debug output
docker compose logs compositor | grep -i error

# Common issues:
# - Missing codec support
# - Invalid video format
# - Network issues (SRT latency too low)
```

## Configuration

### Environment Variables

Configure in [`.env`](.env) file:

- **FALLBACK_SOURCE**: Set to `video` to enable video fallback (default: empty/black screen)
- **VIDEO_TCP_PORT**: Port for video TCP server (default: 1940)

### Code Settings

Key settings in [`compositor.py`](compositor.py):

- **Version**: 2.1.0 (line 25)
- **SRT Port**: 1937 (in `add_srt_elements` method)
- **Video TCP Port**: 1940 (configurable via VIDEO_TCP_PORT env var)
- **Output TCP Port**: 5000 (in `_build_output_stage` method)
- **Resolution**: 1920x1080 @ 30fps (in `_build_fallback_sources` method)
- **Sample Rate**: 48kHz stereo (in `_build_fallback_sources` method)
- **Video Bitrate**: 2500 kbps (in `_build_output_stage` method)
- **Audio Bitrate**: 128 kbps (in `_build_output_stage` method)
- **Fade Duration**: 1000ms (in `_fade` method)
- **Watchdog Timeout**: 200ms (in `watchdog_cb` and `video_watchdog_cb` methods)

To modify these, edit the appropriate method in [`compositor.py`](compositor.py) and rebuild the container.

## Video Fallback Feature

The video fallback feature allows you to display a looping video (e.g., "Be Right Back" screen) instead of a black screen when no SRT feed is active.

### Enabling Video Fallback

1. Set environment variable in `.env`:
   ```bash
   FALLBACK_SOURCE=video
   VIDEO_TCP_PORT=1940
   ```

2. Rebuild and restart compositor:
   ```bash
   docker compose up --build -d compositor
   ```

3. Send video stream using ffmpeg:
   ```bash
   ffmpeg \
     -re -stream_loop -1 \
     -i /home/mulligan/offline.mp4 \
     -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
     -c:a aac -b:a 128k -ar 48000 -ac 2 \
     -f mpegts tcp://localhost:1940
   ```

### Testing Video Fallback

See [`VIDEO_FALLBACK_TESTING.md`](VIDEO_FALLBACK_TESTING.md) for comprehensive testing scenarios including:
- Black screen vs video fallback behavior
- State transitions between black → video → SRT
- TCP reconnection handling
- ffmpeg command examples

### How It Works

1. **TCP Server Mode**: Compositor runs a TCP server on port 1940
2. **Client Connection**: ffmpeg connects as a TCP client and sends MPEG-TS stream
3. **Automatic Reset**: When client disconnects, server resets and waits for new connection
4. **Priority System**: SRT feed always takes priority over video fallback

## Development

### Test locally without Docker

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt-get install -y python3-gi python3-gst-1.0 \
    gstreamer1.0-plugins-{base,good,bad,ugly} \
    gstreamer1.0-libav

# Run directly
cd compositor
python3 compositor.py
```

### Monitor state changes

The compositor prints detailed state transitions and operations:

```
[compositor] Starting v2.0.0...
[build] Creating fallback sources (black + silence)...
[build] ✓ Fallback sources created
[build] Creating shared output stage...
[build] ✓ Output stage created
[build] Linking fallback to output...
[build] ✓ Fallback linked to output
[compositor] ✓ Pipeline PLAYING with fallback output
[compositor] TCP output: tcp://0.0.0.0:5000
[srt] Adding SRT elements to pipeline...
[srt] ✓ SRT elements added, listening on port 1937
[compositor] ✓ Ready - streaming black screen, waiting for SRT on port 1937

# When SRT connects:
[srt] Decodebin pad added: video/x-raw...
[srt] ✓ First SRT connection successful
[srt] Video pad link result: GST_PAD_LINK_OK
[srt] ✓ Video linked to compositor
[fade] Starting fade-in
[fade] Fade finished, new state: SRT_CONNECTED

# When SRT disconnects:
[watchdog] No SRT data for 1.1s, triggering fade-out
[fade] Starting fade-out
[fade] Fade finished, new state: FALLBACK_ONLY
[srt] Restarting SRT elements...
[srt] Removing SRT elements from pipeline...
[srt] ✓ SRT elements removed
```

## Next Steps

Once the compositor is working:
1. Test fade behavior with intermittent connections
2. Verify audio/video sync remains correct
3. Test with different input formats and resolutions
4. Consider adding health check endpoint
5. Add metrics/monitoring for production use