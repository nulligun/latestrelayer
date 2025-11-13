# Compositor Container

GStreamer-based video compositor with hybrid architecture for reliable streaming.

**Version: 2.0.0**

## Features

- **Immediate Startup**: Pipeline starts instantly with black screen + silence output
- **SRT Input**: Listens on port 1937 for incoming SRT streams
- **Automatic Fade Control**:
  - Fades in (1 second) when SRT stream connects
  - Fades out (1 second) after 1 second of no SRT data
  - Returns to black screen fallback when no input
- **Hybrid Architecture**: Separate fallback and SRT pipelines sharing single output
- **TCP Output**: Always streaming to TCP port 5000 (even before SRT connects)
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

```
FALLBACK_ONLY (black) → TRANSITIONING (fade 1s) → SRT_CONNECTED
     ↑                                                  ↓
     └──────────── SRT disconnects (fade 1s) ───────────┘
```

**Key Improvement in v2.0.0**: The output stream is **always active**, even before the first SRT connection. The previous version blocked until SRT connected.

## Architecture

### Hybrid Pipeline Design (v2.0.0)

The compositor uses a **hybrid architecture** with three independent stages:

```
┌─────────────────────────────────────────────────────────────┐
│ FALLBACK SOURCES (Always Running)                          │
│   videotestsrc (black) ──→ videoconvert ──→ compositor     │
│   audiotestsrc (silence) ─→ audioconvert ─→ audiomixer     │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ SRT ELEMENTS (Dynamic - Added on startup)                   │
│   srtserversrc:1937 → decodebin ─┬→ queue → alpha ─────┐   │
│                                   └→ queue → volume ────┤   │
└───────────────────────────────────────────────────────────┼─┘
                                                            │
┌───────────────────────────────────────────────────────────┼─┐
│ SHARED OUTPUT (Always Running)                           │ │
│   compositor ←──────────────────────────────────────────┐│ │
│       ↓                                                  ││ │
│   x264enc → mpegtsmux → tcpserversink:5000              ││ │
│       ↑           ↑                                      ││ │
│   audiomixer ←───────────────────────────────────────────┘│ │
└─────────────────────────────────────────────────────────────┘
```

### Key Components

**Fallback Stage** (always present):
- **videotestsrc**: Black screen (1920x1080@30fps)
- **audiotestsrc**: Silence (48kHz stereo)
- Always linked to output, ensures stream never stops

**SRT Stage** (dynamically managed):
- **srtserversrc**: Listens for SRT on port 1937
- **decodebin**: Automatically decodes incoming streams
- **alpha**: Controls video transparency (0.0 = invisible, 1.0 = opaque)
- **volume**: Controls audio level (0.0 = muted, 1.0 = full)
- Can be removed/re-added without affecting output

**Output Stage** (always running):
- **compositor**: Blends fallback + SRT video
- **audiomixer**: Mixes fallback + SRT audio
- **x264enc**: H.264 encoding with low-latency settings
- **mpegtsmux**: MPEG-TS container
- **tcpserversink**: Always streaming on port 5000

### Benefits of Hybrid Architecture

✅ **Immediate startup** - No waiting for SRT connection
✅ **Reliable output** - Stream never stops, even during SRT issues
✅ **Clean isolation** - SRT problems don't affect fallback or output
✅ **Easy debugging** - Each stage can be inspected independently
✅ **Graceful transitions** - Fade in/out between fallback and SRT

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

Key settings in [`compositor.py`](compositor.py):

- **Version**: 2.0.0 (line 19)
- **SRT Port**: 1937 (in `add_srt_elements` method)
- **TCP Port**: 5000 (in `_build_output_stage` method)
- **Resolution**: 1920x1080 @ 30fps (in `_build_fallback_sources` method)
- **Sample Rate**: 48kHz stereo (in `_build_fallback_sources` method)
- **Video Bitrate**: 2500 kbps (in `_build_output_stage` method)
- **Audio Bitrate**: 128 kbps (in `_build_output_stage` method)
- **Fade Duration**: 1000ms (in `_fade` method)
- **Watchdog Timeout**: 1000ms (in `watchdog_cb` method)

To modify these, edit the appropriate method in [`compositor.py`](compositor.py) and rebuild the container.

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