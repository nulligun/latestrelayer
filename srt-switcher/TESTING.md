# SRT Stream Switcher Testing Guide

## Recent Fix: SRT Connection Detection (v4.3)

### Problem
The SRT switcher was rapidly cycling between detecting and losing the camera feed every ~2.6 seconds, even though the SRT stream was connected and sending data continuously.

### Root Cause
The data flow monitoring probe was attached to the raw `fdsrc` element, which reads from FFmpeg's stdout pipe. This was unreliable because:
- Buffering between FFmpeg → GStreamer caused irregular probe firing
- The probe didn't consistently detect every buffer
- No redundant detection mechanisms

### Solution
**Three-tier monitoring system:**

1. **PRIMARY**: Probe on demuxed video pad (after flvdemux)
   - Monitors decoded 30fps video stream
   - Most reliable detection method
   - Fires consistently for every video frame

2. **SECONDARY**: Probe on demuxed audio pad
   - Redundant detection on audio stream
   - Backup if video probe fails

3. **TERTIARY**: FFmpeg stderr monitoring
   - Detects `frame=` progress messages
   - Ultimate fallback detection

### Testing the Fix

**1. Rebuild and restart the container:**
```bash
docker compose build srt-switcher
docker compose up -d srt-switcher
```

**2. Start your SRT test stream:**
```bash
ffmpeg -re -stream_loop -1 -i /home/mulligan/offline2.mp4 \
  -c:v libx264 -preset veryfast -tune zerolatency \
  -b:v 3000k -maxrate 3000k -bufsize 6000k -g 60 \
  -c:a aac -b:a 128k -ar 48000 \
  -f mpegts "srt://localhost:1937?mode=caller"
```

**3. Monitor the logs:**
```bash
docker compose logs -f srt-switcher
```

**Expected behavior (FIXED):**
```
[gst-simple]   ✅ SRT VIDEO linked: video → sink_1
[gst-simple]   ✅ Video probe attached to demuxed pad
[gst-simple]   ✅ SRT AUDIO linked: audio → sink_1
[gst-simple]   ✅ Audio probe attached to demuxed pad
[monitor] ✓ SRT connection detected (data age: 0.01s)
[auto] SRT feed connected (mode=camera)
[switch] ✓ Active source: SRT (video:sink_1, audio:sink_1)
... stays stable, NO disconnect messages ...
```

**4. Enable verbose probe logging (optional):**
```bash
# Add to docker-compose.yml under srt-switcher environment:
- SRT_MONITOR_VERBOSE=true

docker compose up -d srt-switcher
```

With verbose logging, you'll see:
```
[probe] Video buffers: 30 (29.98 fps)
[probe] Video buffers: 30 (30.02 fps)
[probe] Audio buffer detected (secondary detection active)
```

### Troubleshooting

**If connection still unstable:**

1. **Check actual data flow:**
```bash
# Watch for frame= messages from FFmpeg
docker compose logs -f srt-switcher | grep "frame="
```

2. **Verify network connectivity:**
```bash
# Check if SRT port is accessible
nc -zvu localhost 1937
```

3. **Increase timeout temporarily:**
```bash
# In docker-compose.yml:
environment:
  - SRT_TIMEOUT=5.0  # More tolerant of brief gaps
```

4. **Check probe attachment:**
```bash
# Look for these messages in logs:
docker compose logs srt-switcher | grep "probe attached"
```

Should see:
```
[gst-simple]   ✅ Video probe attached to demuxed pad
[gst-simple]   ✅ Audio probe attached to demuxed pad
```

---

# Original Testing Documentation

## Quick VLC Testing (v4.2 - TCP Only)

The srt-switcher provides a **local TCP server** for immediate testing with VLC. No external RTMP server or streaming credentials needed.

### Connect VLC to Test Stream

**From the same machine:**
```
tcp://localhost:8554
```

**From another machine on the network:**
```
tcp://YOUR_HOST_IP:8554
```

### Steps to Test:

1. **Start the srt-switcher:**
   ```bash
   docker compose up srt-switcher
   ```

2. **Open VLC Media Player:**
   - Go to: Media → Open Network Stream
   - Enter: `tcp://localhost:8554`
   - Click Play

3. **You should see:**
   - The offline/fallback video playing immediately
   - Smooth playback with synchronized audio

4. **Test Input Switching:**
   - **Start SRT source** (e.g., from OBS or another streaming app to port 9000)
   - Watch VLC - it should **automatically switch to the SRT source**
   - **Stop SRT source** - it should **switch back to offline video**

5. **Test Manual Switching:**
   ```bash
   # Force offline mode (privacy)
   curl -X POST http://localhost:8088/scene/privacy
   
   # Enable auto-switching (camera mode)
   curl -X POST http://localhost:8088/scene/camera
   ```

### Expected Behavior:

✅ **Instant playback** - TCP server is always available
✅ **Smooth transitions** - No black screens between sources
✅ **Synchronized A/V** - Audio and video stay in sync
✅ **Multiple viewers** - Multiple VLC instances can connect simultaneously

### Troubleshooting VLC Connection:

**"Unable to open network stream":**
- Verify srt-switcher is running: `docker compose ps`
- Check port 8554 is exposed in docker-compose.yml
- From another machine, use host IP instead of localhost

**Playback stuttering:**
- This is normal if encoding preset is too slow
- Reduce quality or increase preset speed in config

**No video after switch:**
- Check logs: `docker compose logs srt-switcher | grep switch`
- Verify SRT source is actually connecting to port 9000


# SRT Switcher v2.0 - Testing Guide

This guide covers testing the refactored dual-pipeline SRT switcher with emphasis on verifying seamless switching, SRT connections, and output quality.

## Architecture Overview

**New Dual-Pipeline Design:**
```
Offline Video → Decode → Normalize → Encode → FLV Mux ──┐
                                                          ├→ Input Selector → Tee → Fakesink
SRT Listener  → Decode → Normalize → Encode → FLV Mux ──┘                      └→ Valve → RTMP Sink
                                                                                    (Kick)
```

**Key Features:**
- Both pipelines encode independently and continuously
- Seamless switching at the FLV level (no re-encoding)
- SRT monitor thread with 2.5s timeout detection
- Offline video loops automatically when SRT disconnected

## Prerequisites

### Required Tools

1. **FFmpeg** (for sending test streams)
   ```bash
   sudo apt-get install ffmpeg
   ```

2. **FFplay** (for viewing output)
   ```bash
   # Usually included with ffmpeg
   ffplay -version
   ```

3. **curl** (for API testing)
   ```bash
   sudo apt-get install curl
   ```

4. **OBS Studio** (optional, for real-world testing)
   - Download from https://obsproject.com/

### Test Video File

Ensure you have a fallback video mounted:
```bash
# Check if video exists
ls -lh /videos/fallback.mp4

# If not, you can use any video file
docker cp /path/to/your/video.mp4 srt-switcher:/videos/fallback.mp4
```

## Starting the Switcher

### Using Docker Compose

```bash
# Start the switcher
docker compose --profile test up -d srt-switcher

# View logs with timestamps
docker compose logs -f srt-switcher

# Check initial status
curl http://localhost:9088/health | jq
```

**Expected Initial Output:**
```
[offline] Building offline video pipeline from: /videos/fallback.mp4
[srt] Building SRT pipeline on port 9000...
[output] Building selector and output pipeline...
[monitor] Starting SRT connection monitor
[pipeline] State: null → playing
[pipeline] ✓✓✓ Pipeline is PLAYING ✓✓✓
[main] Offline video playing, waiting for SRT connections...
```

## Testing SRT Connections

### Test 1: Basic SRT Connection with FFmpeg

**Send a test stream:**
```bash
# Using a test video file
ffmpeg -re -i test.mp4 \
  -c:v libx264 -preset veryfast -tune zerolatency -b:v 2000k \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:9937?mode=caller"
```

**Using a webcam (Linux):**
```bash
ffmpeg -f v4l2 -i /dev/video0 \
  -f alsa -i default \
  -c:v libx264 -preset veryfast -tune zerolatency -b:v 2000k \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:9937?mode=caller"
```

**Using screen capture:**
```bash
ffmpeg -f x11grab -video_size 1920x1080 -framerate 30 -i :0.0 \
  -f pulse -i default \
  -c:v libx264 -preset veryfast -tune zerolatency -b:v 3000k \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:9937?mode=caller"
```

**Expected Log Output:**
```
[srt] ✓ Video pad linked - SRT stream connected!
[srt] ✓ Audio pad linked
[monitor] ✓ SRT connection detected (data age: 0.15s)
[auto] SRT feed connected (mode=camera)
[auto] Camera mode: switching to live source
[switch] ✓ Active source: SRT (pad: sink_1)
```

### Test 2: Connection Monitoring

**Monitor the health endpoint:**
```bash
# In a separate terminal
watch -n 1 'curl -s http://localhost:9088/health | jq'
```

**You should see:**
```json
{
  "status": "healthy",
  "current_source": "srt",
  "srt_connected": true,
  "kick_streaming_enabled": false,
  "scene_mode": "camera",
  "pipeline_state": "playing",
  "uptime_seconds": 45
}
```

### Test 3: SRT Disconnect Detection

**Stop the FFmpeg stream** (Ctrl+C) and watch the logs:

**Expected Output:**
```
[monitor] ✗ SRT connection lost (no data for 2.51s)
[auto] SRT feed disconnected (mode=camera)
[auto] Camera mode: switching to offline video
[switch] ✓ Active source: Offline (pad: sink_0)
```

**Verify in health endpoint:**
```json
{
  "status": "healthy",
  "current_source": "offline",
  "srt_connected": false,
  ...
}
```

### Test 4: Rapid Connect/Disconnect Cycles

**Test the debounce logic:**
```bash
# Start stream for 5 seconds, stop for 5 seconds, repeat
for i in {1..3}; do
  echo "Cycle $i: Starting stream..."
  timeout 5 ffmpeg -re -i test.mp4 -c:v libx264 -c:a aac \
    -f mpegts "srt://localhost:9937?mode=caller" 2>/dev/null
  
  echo "Cycle $i: Stopped, waiting 5s..."
  sleep 5
done
```

**Expected Behavior:**
- Switch to SRT immediately when detected
- Wait 2.5s before switching back to offline
- No flickering or rapid switching

## Testing Manual Controls

### Manual Source Switching

**Switch to SRT manually:**
```bash
curl "http://localhost:9088/switch?src=srt"
```

**Switch to offline manually:**
```bash
curl "http://localhost:9088/switch?src=offline"
# or
curl "http://localhost:9088/switch?src=fallback"
```

**Check current scene:**
```bash
curl http://localhost:9088/scene
# Output: {"scene": "srt"} or {"scene": "offline"}
```

### Scene Mode Testing

**Enable Privacy Mode (always offline):**
```bash
curl -X POST http://localhost:9088/scene/privacy

# Expected: Switches to offline even if SRT connected
# Logs: [scene] Privacy mode: forcing offline video
```

**Enable Camera Mode (auto-switch):**
```bash
curl -X POST http://localhost:9088/scene/camera

# Expected: Switches to SRT if connected
# Logs: [scene] Camera mode: switching to SRT (connected)
```

**Check current mode:**
```bash
curl http://localhost:9088/scene/mode
# Output: {"mode": "camera"} or {"mode": "privacy"}
```

## Output Verification

### Method 1: Using FFplay

**View the output stream:**
```bash
# If you have a local RTMP server
ffplay -fflags nobuffer -flags low_delay \
  rtmp://localhost/live/output

# If testing Kick URL (won't work, but shows connection)
# You'll need to view on Kick's website
```

### Method 2: Monitor Logs

**Look for these indicators of healthy output:**
```
✓ Pipeline is PLAYING
✓ Active source: [srt|offline]
✓ Kick streaming enabled (valve opened)
```

### Method 3: Using GStreamer Debug

**Enable GStreamer debug for more details:**
```bash
docker compose down srt-switcher

# Edit docker-compose.yml to add:
# environment:
#   - GST_DEBUG=3

docker compose --profile test up srt-switcher
```

### Method 4: Overall Health

**Check overall health endpoints:**
```bash
curl "http://localhost:9088/health"
curl "http://localhost:9088/health/details"
```

## Troubleshooting

### Problem: Offline video doesn't play

This issue may be caused by caps negotiation failures, video file compatibility,
or file integrity problems.

**Check logs for:**
```
[offline] Building offline video pipeline from: /videos/fallback.mp4
[offline] ✓ Video pad linked
[offline] ✓ Audio pad linked
```

**Common Issue: Caps Negotiation Failure**

If you see errors like:
```
[ERROR] qtdemux0: gst-stream-error-quark: Internal data stream error. (1)
streaming stopped, reason not-negotiated (-4)
[pipeline] State: ready → paused
```

This indicates GStreamer couldn't negotiate capabilities between pipeline elements.

**Solutions:**

1. **Verify video file format:**
```bash
# Check video file with ffprobe
docker exec srt-switcher ffprobe /videos/fallback.mp4

# Look for:
# - Video codec: h264, mpeg4, or other common codec
# - Audio codec: aac, mp3, or pcm
# - Container: MP4, AVI, or MKV
```

2. **Test with GStreamer directly:**
```bash
# Basic playback test
docker exec srt-switcher gst-launch-1.0 filesrc location=/videos/fallback.mp4 ! decodebin ! fakesink

# If this fails, your video file may be corrupted or in an unsupported format
```

3. **Enable GStreamer debug logging:**
```bash
# In docker-compose.yml, add to environment:
GST_DEBUG=3  # or higher for more detail (1-9)

# Restart container and check logs for detailed caps negotiation info
docker compose restart srt-switcher
docker compose logs -f srt-switcher
```

4. **Video format requirements:**
   - **Supported codecs:** H.264, MPEG-4, H.265, VP8, VP9, Theora
   - **Supported audio:** AAC, MP3, Vorbis, Opus, PCM
   - **Recommended format:** H.264 + AAC in MP4 container
   - **File integrity:** Ensure file is not corrupted

5. **Convert video to compatible format:**
```bash
# Using ffmpeg to create a compatible fallback video
ffmpeg -i input.mp4 \
  -c:v libx264 -preset fast -crf 23 \
  -c:a aac -b:a 128k \
  -movflags +faststart \
  fallback.mp4
```

**If missing, verify:**
```bash
# File exists?
docker exec srt-switcher ls -l /videos/fallback.mp4

# File is readable?
docker exec srt-switcher file /videos/fallback.mp4

# GStreamer can decode it?
docker exec srt-switcher gst-launch-1.0 filesrc location=/videos/fallback.mp4 ! decodebin ! fakesink
```

### Problem: SRT connection not detected

**Verify port mapping:**
```bash
docker compose ps srt-switcher
# Should show: 0.0.0.0:9937->9000/udp
```

**Test connectivity:**
```bash
# From host machine
nc -u -v localhost 9937

# From inside container
docker exec srt-switcher netstat -uln | grep 9000
```

**Check FFmpeg SRT settings:**
```bash
# Ensure mode=caller (not listener)
# Check latency settings if needed
ffmpeg ... -f mpegts "srt://localhost:9937?mode=caller&latency=100000"
```

### Problem: No switch when SRT connects

**Check scene mode:**
```bash
curl http://localhost:9088/scene/mode
# If "privacy", switch won't happen
```

**Watch monitor thread:**
```bash
docker compose logs -f srt-switcher | grep monitor
```

**Expected:**
```
[monitor] ✓ SRT connection detected (data age: 0.25s)
```

### Problem: Switching happens too slowly/quickly

**Adjust SRT_TIMEOUT:**
```bash
# In docker-compose.yml or .env
export SRT_TIMEOUT=1.5  # Faster detection
# or
export SRT_TIMEOUT=5.0  # Slower detection (more tolerant)
```

### Problem: Stream quality issues

**Check encoding settings:**
```bash
# View current config
docker exec srt-switcher env | grep OUTPUT
```

**Adjust if needed in docker-compose.yml:**
```yaml
environment:
  - OUTPUT_BITRATE=4000  # Increase for better quality
  - OUTPUT_FPS=60        # Increase for smoother motion
```

## Performance Testing

### Monitor Resource Usage

```bash
# CPU and Memory
docker stats srt-switcher

# Should see:
# - CPU: 20-40% (two encoders running)
# - Memory: 200-400MB
```

### Network Bandwidth

```bash
# Monitor input (SRT)
docker exec srt-switcher iftop -i eth0

# Output should show:
# - Inbound: ~2-4 Mbps (when SRT connected)
# - Outbound: ~3 Mbps (when Kick streaming enabled)
```

## Complete Test Sequence

**Run this complete test to verify all functionality:**

```bash
#!/bin/bash

echo "=== SRT Switcher v2.0 - Complete Test Sequence ==="

# 1. Check initial state
echo -e "\n1. Checking initial state..."
curl -s http://localhost:9088/health | jq '.current_source, .srt_connected'

# 2. Start SRT stream
echo -e "\n2. Starting SRT test stream..."
timeout 10 ffmpeg -re -f lavfi -i testsrc=size=1920x1080:rate=30 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -c:v libx264 -preset veryfast -tune zerolatency -b:v 3000k \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:9937?mode=caller" &>/dev/null &
FFMPEG_PID=$!

sleep 5

# 3. Verify SRT connection
echo -e "\n3. Verifying SRT connection..."
curl -s http://localhost:9088/health | jq '.current_source, .srt_connected'

# 4. Test manual switch
echo -e "\n4. Testing manual switch to offline..."
curl -s "http://localhost:9088/switch?src=offline"
sleep 2
curl -s http://localhost:9088/health | jq '.current_source'

# 5. Test privacy mode
echo -e "\n5. Testing privacy mode..."
curl -s -X POST http://localhost:9088/scene/privacy | jq
sleep 2
curl -s http://localhost:9088/health | jq '.scene_mode, .current_source'

# 6. Back to camera mode
echo -e "\n6. Switching back to camera mode..."
curl -s -X POST http://localhost:9088/scene/camera | jq
sleep 2
curl -s http://localhost:9088/health | jq '.scene_mode, .current_source'

# 8. Stop SRT stream and verify fallback
echo -e "\n8. Stopping SRT stream..."
kill $FFMPEG_PID 2>/dev/null
sleep 4

echo -e "\n9. Verifying fallback to offline..."
curl -s http://localhost:9088/health | jq '.current_source, .srt_connected'

echo -e "\n=== Test Sequence Complete ==="
```

**Save this as `test_switcher.sh` and run:**
```bash
chmod +x test_switcher.sh
./test_switcher.sh
```

## Expected Results Summary

✅ **Healthy System:**
- Offline video plays immediately on startup
- SRT connection detected within 1 second
- Switch to SRT happens automatically in camera mode
- Switch back to offline happens 2.5s after SRT disconnect
- Manual controls work immediately
- Privacy mode prevents auto-switching
- Kick streaming can be enabled/disabled without pipeline restart

❌ **Problem Indicators:**
- "Pipeline state: paused" in health check
- No log output from monitor thread
- Source doesn't change after SRT connection
- Video doesn't loop in offline mode
- Errors in GStreamer bus messages

## Next Steps

After verifying the switcher works correctly:

1. **Update main docker-compose.yml** to use standard ports
2. **Remove old containers** (nginx-rtmp, ffmpeg-*, muxer)
3. **Update dashboard** to point to new API
4. **Deploy to production** with confidence!

---

For issues or questions, check the main logs:
```bash
docker compose logs -f srt-switcher
```