# Video Fallback Testing Guide

## Overview

The video fallback feature allows the compositor to display a looping video when no SRT feed is available. This guide covers testing scenarios and commands.

## Prerequisites

- Compositor running with `FALLBACK_SOURCE=video` in `.env`
- Video file available (e.g., `/home/mulligan/offline.mp4`)
- ffmpeg installed on your system

## FFmpeg Connection Methods

### Method 1: From Host (Outside Docker)

Connect to the exposed port on localhost:

```bash
ffmpeg \
  -re -stream_loop -1 \
  -i /home/mulligan/offline.mp4 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts tcp://localhost:1940
```

### Method 2: From Another Docker Container

Connect using Docker networking:

```bash
ffmpeg \
  -re -stream_loop -1 \
  -i /path/to/video.mp4 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts tcp://compositor:1940
```

## Testing Scenarios

### Scenario 1: Black Screen Fallback (Default Behavior)

**Test:** Verify black screen works when video fallback is disabled

```bash
# In .env, set or comment out:
# FALLBACK_SOURCE=
# or remove the line entirely

# Start compositor
docker compose up compositor

# Expected: Black screen on tcp://localhost:5000
# No TCP server on port 1940
```

### Scenario 2: Video Fades In From Black

**Test:** Video should fade in when TCP client connects

```bash
# In .env:
FALLBACK_SOURCE=video
VIDEO_TCP_PORT=1940

# 1. Start compositor
docker compose up compositor

# 2. Wait for message: "waiting for video TCP on port 1940"

# 3. Start ffmpeg (from host)
ffmpeg \
  -re -stream_loop -1 \
  -i /home/mulligan/offline.mp4 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts tcp://localhost:1940

# Expected compositor logs:
# [video] Decodebin pad added: video/...
# [video] ✓ First TCP video connection successful
# [fade] Starting fade-in (video)
# [fade] Step 0/10: video_pad_alpha=0.00, vol=0.00, state=VIDEO_TRANSITIONING
# ...
# [fade] Step 10/10: video_pad_alpha=1.00, vol=1.00, state=VIDEO_TRANSITIONING
# [fade] Fade finished, new state: VIDEO_CONNECTED

# Expected output: Video fades in over 1 second
```

### Scenario 3: SRT Fades In Over Video

**Test:** SRT feed should take priority over video fallback

```bash
# Prerequisites: Video fallback is running (Scenario 2)

# Send SRT feed using OBS or ffmpeg:
ffmpeg \
  -re -f lavfi -i testsrc=size=1920x1080:rate=30 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -c:v libx264 -preset veryfast -tune zerolatency \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:1937?mode=caller"

# Expected compositor logs:
# [srt] Decodebin pad added: video/...
# [srt] ✓ First SRT connection successful
# [fade] Starting fade-in (SRT)
# [fade] Step 0/10: srt_pad_alpha=0.00, vol=0.00, state=SRT_TRANSITIONING
# ...
# [fade] Fade finished, new state: SRT_CONNECTED

# Expected output: SRT feed fades in over video fallback
```

### Scenario 4: Fade Back to Video When SRT Disconnects

**Test:** Should return to video when SRT disconnects, video still running

```bash
# Prerequisites: SRT is active over video (Scenario 3)

# Stop the SRT ffmpeg process (Ctrl+C)

# Expected compositor logs:
# [watchdog] No SRT data for 0.2s, state=SRT_CONNECTED, triggering fade-out
# [fade] Starting fade-out (SRT → video)
# [fade] Step 0/10: srt_pad_alpha=1.00, vol=1.00, state=SRT_TRANSITIONING
# ...
# [fade] Fade finished, new state: VIDEO_CONNECTED
# [srt] Restarting SRT elements...

# Expected output: SRT fades out, video fallback visible again
```

### Scenario 5: Fade to Black When TCP Disconnects

**Test:** Should return to black screen when video TCP disconnects

```bash
# Prerequisites: Video fallback is running, no SRT (Scenario 2)

# Stop the video ffmpeg process (Ctrl+C)

# Expected compositor logs:
# [watchdog] No video TCP data for 0.2s, state=VIDEO_CONNECTED, triggering fade-out
# [fade] Starting fade-out (video)
# [fade] Step 0/10: video_pad_alpha=1.00, vol=1.00, state=VIDEO_TRANSITIONING
# ...
# [fade] Fade finished, new state: FALLBACK_ONLY
# [video] Restarting video TCP server...
# [video] ✓ Video TCP server restart complete, waiting for connection...

# Expected output: Video fades out to black screen over 1 second
# TCP server resets and waits for new connection
```

### Scenario 6: TCP Reconnection

**Test:** Video should fade back in when TCP reconnects

```bash
# Prerequisites: TCP disconnected and server reset (Scenario 5)

# Restart ffmpeg with same command as Scenario 2
ffmpeg \
  -re -stream_loop -1 \
  -i /home/mulligan/offline.mp4 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts tcp://localhost:1940

# Expected compositor logs:
# [video] Decodebin pad added: video/...
# [fade] Starting fade-in (video)
# ...
# [fade] Fade finished, new state: VIDEO_CONNECTED

# Expected output: Video fades in from black screen
```

## State Transition Diagram

```
FALLBACK_ONLY (Black)
    ↓ TCP connects
VIDEO_TRANSITIONING (Fading in)
    ↓
VIDEO_CONNECTED (Video visible)
    ↓ SRT connects           ↓ TCP disconnects
SRT_TRANSITIONING ←→ VIDEO_TRANSITIONING
    ↓                        ↓
SRT_CONNECTED            FALLBACK_ONLY
    ↓ SRT disconnects
VIDEO_TRANSITIONING (if video still connected)
  or
FALLBACK_ONLY (if video disconnected)
```

## Monitoring Commands

### View Compositor Logs
```bash
docker compose logs -f compositor
```

### View Output Stream
```bash
ffplay tcp://localhost:5000
# or
vlc tcp://localhost:5000
```

### Check TCP Connections
```bash
# Check if port 1940 is listening
netstat -tan | grep 1940

# Check active connections
ss -tn | grep 1940
```

## Troubleshooting

### Video Not Appearing
1. Check `FALLBACK_SOURCE=video` in `.env`
2. Verify port 1940 is exposed in `docker-compose.yml`
3. Check compositor logs for "Video elements added"
4. Ensure ffmpeg is using correct codec parameters

### TCP Connection Refused
1. Verify compositor is running
2. Check port mapping: `docker compose ps`
3. Try connecting from host first before container

### Video Not Looping
1. Ensure `-stream_loop -1` flag in ffmpeg command
2. Check ffmpeg output for errors
3. Verify video file is accessible

### Fade Not Smooth
1. Check compositor logs for buffer flow
2. Verify video bitrate matches expectations
3. Ensure adequate system resources

## Performance Notes

- Video decoding happens in compositor container
- Recommended video bitrate: 2-4 Mbps
- Resolution normalized to 1920x1080@30fps
- Audio normalized to 48kHz stereo
- Fade duration: 1 second (configurable in code)
- Watchdog timeout: 200ms for quick detection