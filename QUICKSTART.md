# Quick Start Guide

Get the TSDuck multiplexer running in 5 minutes!

## Prerequisites

- Docker and Docker Compose installed
- Environment variables configured (see below)

## Steps

### 1. Configure Environment

Create or update your `.env` file with the shared folder path:
```bash
SHARED_FOLDER=/path/to/your/shared/folder
```

The system will automatically generate a default fallback video (black screen with "BRB..." text) on first startup if one doesn't exist.

**Optional: Custom Fallback Video**
If you want to use your own fallback video instead of the auto-generated one, place it in the shared folder as `fallback.ts`:
```bash
# Convert your video to MPEG-TS format (keyframe every 0.5s for fast switching)
ffmpeg -i /path/to/your/video.mp4 \
  -c:v libx264 -preset fast -crf 23 \
  -g 15 -keyint_min 15 -sc_threshold 0 \
  -c:a aac -b:a 128k \
  -bsf:v h264_mp4toannexb \
  -f mpegts -mpegts_flags +resend_headers \
  $SHARED_FOLDER/fallback.ts
```

### 2. Start the System

The system includes an **integrated nginx-rtmp server**, so no external RTMP server configuration is needed for testing!

```bash
# Build containers (first time only)
docker-compose build

# Start all services
docker-compose up -d

# View logs
docker-compose logs -f multiplexer
```

Expected output:
```
[Multiplexer] Configuration loaded successfully
[Live] Started on UDP port 10000
[Fallback] Started on UDP port 10001
[Multiplexer] Live stream: Video PID: 256, Audio PID: 257
[RTMPOutput] Started FFmpeg process
```

### 3. Send Live Video

Using FFmpeg:
```bash
ffmpeg -re -i input.mp4 \
  -c copy \
  -f mpegts "srt://YOUR-SERVER-IP:1937?mode=caller"
```

Using OBS Studio:
1. Settings → Stream
2. Service: Custom
3. Server: `srt://YOUR-SERVER-IP:1937`
4. Mode: Caller

### 4. Watch the Stream

**RTMP Playback** (Low latency ~2-5s):
```bash
# Using VLC
vlc rtmp://localhost:1935/live/stream

# Using ffplay
ffplay rtmp://localhost:1935/live/stream
```

**HLS Playback** (Browser compatible ~10-15s):
```bash
# Direct URL in browser
http://localhost:8080/hls/stream.m3u8

# Or use VLC
vlc http://localhost:8080/hls/stream.m3u8
```

**Monitor Statistics**:
```bash
# Open in browser
http://localhost:8080/stat
```

You should see:
- Live video when SRT input is active
- Fallback video when SRT stops (after 2 seconds)
- Automatic switch back to live when SRT resumes

## Testing Failover

1. **Start with live video:**
   ```bash
   ffmpeg -re -i test.mp4 -c copy -f mpegts "srt://localhost:1937?mode=caller"
   ```

2. **Stop the FFmpeg process** (Ctrl+C)

3. **Watch the logs:**
   ```
   [StreamSwitcher] LIVE → FALLBACK (gap=2100ms)
   ```

4. **Restart the FFmpeg process**

5. **Watch the logs:**
   ```
   [StreamSwitcher] FALLBACK → LIVE (consecutive packets=10)
   ```

## Stopping

```bash
# Stop all services
docker-compose down

# Stop and remove volumes
docker-compose down -v
```

## Troubleshooting

### "Failed to start live receiver"
- Port 10000 already in use
- Solution: Change `live_udp_port` in config.yaml

### "No RTMP output"
- Check nginx-rtmp is running: `docker ps | grep nginx-rtmp`
- Test playback: `ffplay rtmp://localhost:1935/live/stream`
- Check statistics: Open `http://localhost:8080/stat` in browser
- Check FFmpeg logs: `docker-compose logs multiplexer`
- Check nginx logs: `docker-compose logs nginx-rtmp`

### "Stuck in fallback mode"
- Verify SRT stream is reaching port 1937
- Check: `docker-compose logs ffmpeg-srt-live`
- Ensure live stream has valid PAT/PMT

### "Container won't start"
- Check Docker daemon is running
- Verify SHARED_FOLDER is set in .env file
- Check logs: `docker-compose logs`

### "Fallback stream has no video/audio"
- Check the ffmpeg-fallback container logs: `docker-compose logs ffmpeg-fallback`
- The fallback.ts should be auto-generated on first startup
- Verify the shared folder is mounted correctly

## Stream URLs Reference

| Purpose | URL | Protocol | Latency |
|---------|-----|----------|---------|
| **Playback (RTMP)** | `rtmp://localhost:1935/live/stream` | RTMP | ~2-5s |
| **Playback (HLS)** | `http://localhost:8080/hls/stream.m3u8` | HTTP/HLS | ~10-15s |
| **Statistics HTML** | `http://localhost:8080/stat` | HTTP | Real-time |
| **Statistics XML** | `http://localhost:8080/stat.xml` | HTTP | Real-time |
| **Live Input** | `srt://localhost:1937` | SRT | N/A |

## Next Steps

- Read [README.md](README.md) for detailed documentation
- Review [ARCHITECTURE.md](ARCHITECTURE.md) for technical details
- Monitor streams at `http://localhost:8080/stat`
- For production: Configure external RTMP URL in `config.yaml`

## Common Configurations

### Production Settings

```yaml
live_udp_port: 10000
fallback_udp_port: 10001
rtmp_url: "rtmp://production-server/live/stream1"
max_live_gap_ms: 3000  # More tolerant for network issues
log_level: "INFO"
```

### Development/Testing (Built-in nginx-rtmp)

```yaml
live_udp_port: 10000
fallback_udp_port: 10001
rtmp_url: "rtmp://nginx-rtmp/live/stream"  # Internal Docker network
max_live_gap_ms: 1000  # Quick failover for testing
log_level: "DEBUG"
```

### External RTMP Server (YouTube, Twitch, etc.)

```yaml
live_udp_port: 10000
fallback_udp_port: 10001
rtmp_url: "rtmp://a.rtmp.youtube.com/live2/YOUR-STREAM-KEY"
max_live_gap_ms: 2000
log_level: "INFO"
```

## Performance Tips

1. **Use hardware encoding for live input** to reduce CPU load
2. **Match fallback bitrate** to live stream for smooth transitions
3. **Monitor queue sizes** in logs to detect bottlenecks
4. **Use SSD storage** for fallback video file

## Support

For issues, check:
1. Container logs: `docker-compose logs`
2. Individual service logs: `docker-compose logs [service-name]`
3. README.md troubleshooting section
4. ARCHITECTURE.md for technical details