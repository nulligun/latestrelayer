# Compositor Test Commands

## Send Test Stream (Looping)

Use this command to stream `/home/mulligan/offline2.mp4` in a loop to the compositor:

```bash
ffmpeg -re -stream_loop -1 -i /home/mulligan/offline2.mp4 \
  -c:v libx264 -preset ultrafast -tune zerolatency -b:v 3000k \
  -c:a aac -b:a 128k \
  -f mpegts "srt://localhost:1937?mode=caller&latency=2000"
```

**Parameters:**
- `-re` - Read input at native frame rate (realtime)
- `-stream_loop -1` - Loop video infinitely
- `-c:v libx264 -preset ultrafast` - Fast H.264 encoding
- `-tune zerolatency` - Optimize for low latency
- `-b:v 3000k` - Video bitrate 3000 kbps
- `-c:a aac -b:a 128k` - AAC audio at 128 kbps
- `-f mpegts` - MPEG-TS container format
- `srt://localhost:1937` - Send to compositor SRT port

## View Output with VLC

**Command line:**
```bash
vlc tcp://localhost:5000
```

**Or in VLC GUI:**
1. Open VLC
2. Media → Open Network Stream
3. Enter: `tcp://localhost:5000`
4. Click Play

## Expected Behavior

1. **Before FFmpeg connects**: Black screen on VLC
2. **FFmpeg connects**: Video fades in over 1 second
3. **While streaming**: Video plays normally at full opacity
4. **FFmpeg disconnects**: After 1 second, video fades out over 1 second
5. **After disconnect**: Black screen with last frame frozen

## Check Logs

```bash
# Watch logs in real-time
docker compose logs -f compositor

# Look for:
# - "Starting fade-in" when SRT connects
# - "Fade finished, new state: LIVE" when fade completes
# - "Starting fade-out" when stream disconnects
# - "Fade finished, new state: FALLBACK" when fade completes
```

## Troubleshooting

### No video in VLC
- Verify compositor is running: `docker compose ps compositor`
- Check logs: `docker compose logs compositor`
- Restart VLC and reconnect

### FFmpeg can't connect
- Verify port 1937 is exposed: `docker compose ps compositor`
- Check firewall settings
- Try with explicit IP: `srt://127.0.0.1:1937`

### Black screen only
- Check if FFmpeg is actually sending: look for "Stream mapping" in FFmpeg output
- Verify FFmpeg is still running (not crashed)
- Check compositor logs for "decodebin pad added" messages