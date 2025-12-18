# MPEG-TS Stream Splicer

A lightweight MPEG-TS splicer with automatic failover. Works in the **uncompressed domain** (no transcoding), so it runs on extremely low-end hardware—think **$5/month VPS** for an always-online streaming server with failover.

## Why This Exists

Traditional stream switching requires transcoding, which needs expensive CPU. This splicer operates directly on MPEG-TS packets, splicing streams without decoding. Result: minimal CPU usage, cheap hosting.

## Quick Start

### 1. Install Docker

```bash
# Ubuntu/Debian
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
# Log out and back in
```

### 2. Configure

```bash
# Copy the example environment file
cp env.default .env

# Edit with your settings
nano .env
```

Key settings in `.env`:
```bash
SRT_INPUT_PORT=1935          # Where you send your stream
RTMP_OUTPUT_URL=rtmp://...   # Where the output goes
```

### 3. Run

```bash
docker compose up -d
```

That's it. Send an SRT stream to port 1935, and the splicer outputs to your configured RTMP destination with automatic failover to a fallback video if your source drops.

## How It Works

```
SRT Input → [Splicer] → RTMP Output (via SRS)
              ↑
        Fallback Video
        (auto-switch on dropout)
```

- **Live stream drops?** Automatically switches to fallback
- **Live stream returns?** Automatically switches back
- **Timestamps?** Kept continuous across switches

## Monitoring

- **SRS Console**: `http://localhost:8080` - Stream stats and playback
- **Dashboard**: `http://localhost:3000` - Container management

## Ports

| Port | Service |
|------|---------|
| 1935 | SRT input |
| 1936 | RTMP output (SRS) |
| 8080 | SRS web console |
| 3000 | Dashboard |

## License

MIT
