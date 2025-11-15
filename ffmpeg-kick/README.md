# FFmpeg Kick Container

Streams video from the compositor to Kick.com (via Amazon IVS/RTMPS).

## Overview

This container receives the mixed video stream from the compositor via TCP and pushes it to Kick.com using RTMPS. It's designed to work seamlessly with the dashboard's Kick Settings UI.

## Features

- **Automatic Reconnection**: Exponential backoff retry logic (1s → 2s → 4s → 8s → 10s max)
- **Zero Re-encoding**: Uses `-c:v copy -c:a copy` for minimal latency
- **Health Monitoring**: Validates ffmpeg process is running
- **Secure Credentials**: Reads from mounted config file, never logs sensitive data
- **Manual Profile**: Only starts when explicitly requested via dashboard

## Configuration

### Via Dashboard UI (Recommended)

1. Navigate to the **Kick Settings** section in the dashboard
2. Enter your Kick Stream URL (RTMPS endpoint)
3. Enter your Kick Stream Key
4. Click **Save Configuration**
5. Use the **Stream to Kick** toggle to start/stop streaming

### Via Config File

Credentials are stored in `shared/kick_config.json`:

```json
{
  "kickUrl": "rtmps://your-endpoint.live-video.net/app",
  "kickKey": "sk_us-west-2_...",
  "lastUpdated": "2025-11-14T21:00:00.000Z"
}
```

### Fallback to Environment Variables

If `kick_config.json` doesn't exist, the system will use values from `.env`:

```env
KICK_URL=rtmps://fa723fc1b171.global-contribute.live-video.net/app
KICK_KEY=sk_us-west-2_xXX3uY9mOSJP_eZTBNAACIwJSKv3EMu6Dhh2La1XZ1s
```

## Usage

### Starting the Stream

**Via Dashboard:**
- Click the **Stream to Kick** toggle
- Confirm the action in the modal

**Via API:**
```bash
curl -X POST http://localhost:3000/api/kick/start
```

**Via CLI:**
```bash
docker compose start ffmpeg-kick
```

## Stopping the Stream

**Via Dashboard:**
- Click the **Stream to Kick** toggle again
- Confirm the action in the modal

**Via API:**
```bash
curl -X POST http://localhost:3000/api/kick/stop
```

**Via CLI:**
```bash
docker compose stop ffmpeg-kick
```

## Architecture

```
┌─────────────┐ TCP:5000  ┌──────────────┐ RTMPS   ┌──────────┐
│ Compositor  │──────────>│ ffmpeg-kick  │───────>│ Kick.com │
│  (GStreamer)│          │  (FFmpeg)     │         │   (IVS)  │
└─────────────┘          └──────────────┘         └──────────┘
                               ▲
                               │ Config
                         ┌─────────────┐
                         │kick_config  │
                         │   .json     │
                         └─────────────┘
```

## FFmpeg Command

The container uses this optimized command:

```bash
ffmpeg \
  -re \
  -i tcp://compositor:5000 \
  -c:v copy \
  -c:a copy \
  -f flv "${KICK_URL}/${KICK_KEY}"
```

**Parameters:**
- `-re`: Read input at native frame rate
- `-i tcp://compositor:5000`: Input from compositor TCP stream
- `-c:v copy`: Copy video codec (no re-encoding)
- `-c:a copy`: Copy audio codec (no re-encoding)
- `-f flv`: Output format FLV for RTMP/RTMPS

## Health Checks

Health checks run every 10 seconds and verify the ffmpeg process is active:

```bash
pgrep -x "ffmpeg" > /dev/null
```

**Health States:**
- ✅ **healthy**: ffmpeg is running
- ❌ **unhealthy**: ffmpeg process not found (3 consecutive failures)
- 🟡 **starting**: Container starting up (10s grace period)

## Troubleshooting

### Container won't start

1. Check if config file exists:
   ```bash
   cat shared/kick_config.json
   ```

2. Verify credentials in dashboard Kick Settings

3. Check container logs:
   ```bash
   docker compose logs ffmpeg-kick
   ```

### Stream disconnects frequently

1. Check compositor is running:
   ```bash
   docker compose ps compositor
   ```

2. Verify network connectivity to Kick:
   ```bash
   docker compose exec ffmpeg-kick ping -c 3 fa723fc1b171.global-contribute.live-video.net
   ```

3. Review ffmpeg errors in logs

### "Config file not found" error

The container expects `kick_config.json` in the shared folder. Either:
- Configure via dashboard UI (creates file automatically)
- Create manually from template above
- Ensure `.env` has `KICK_URL` and `KICK_KEY` set

## Security Notes

- Credentials are **never logged** in clear text
- Stream key shown as `[REDACTED]` in all logs
- Dashboard fields are obscured by default (password-style)
- Click eye icon to temporarily reveal credentials
- Config file mounted **read-only** to container
- Credentials auto-hide after saving in UI

## Integration with Simplified View

When using the **Simplified View** interface:
- **Go Live** button starts both streaming to Kick
- **End Stream** button stops streaming
- Same underlying `/api/kick/start` and `/api/kick/stop` endpoints

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `COMPOSITOR_HOST` | `compositor` | Hostname of compositor service |
| `COMPOSITOR_PORT` | `5000` | TCP port to receive stream from |
| `CONFIG_PATH` | `/app/shared/kick_config.json` | Path to credentials config |
| `RETRY_DELAY` | `1` | Initial retry delay in seconds |
| `MAX_RETRY_DELAY` | `10` | Maximum retry delay in seconds |

## Related Documentation

- [Compositor README](../compositor/README.md)
- [Stream Dashboard README](../stream-dashboard/README.md)
- [Docker Compose Configuration](../docker-compose.yml)