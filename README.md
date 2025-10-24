# RTMP Relay System with Persistent Connection

A Docker Compose-based RTMP relay system that maintains a continuous connection to Kick.com (IVS) while seamlessly switching between live camera feed and offline video without stream interruption.

## Features

✅ **Persistent Connection** - Single FFmpeg process maintains continuous connection to Kick  
✅ **Seamless Switching** - Uses FFmpeg streamselect/aselect filters with ZMQ control  
✅ **Zero Downtime** - No stream disconnection when switching between live and offline  
✅ **Automatic Failover** - Detects live stream status and switches automatically  
✅ **External Publisher Detection** - Only switches to live when non-private IP publishes  
✅ **Containerized** - All components run in isolated Docker containers  
✅ **Configurable** - All settings in `.env` file  
✅ **Observable** - Comprehensive logging for all components  

## Architecture

```
┌─────────────┐
│ Camera/OBS  │
└──────┬──────┘
       │ Publishes RTMP
       ▼
┌──────────────────┐
│  nginx-rtmp      │◄──┐
│  Port: 1936      │   │
│  Stats: 8080     │   │ Polls stats
└────────┬─────────┘   │ (every 1s)
         │             │
         │ Live RTMP   │
         │ Feed        │
         ▼             │
┌──────────────────┐   │
│  ffmpeg-relay    │   │
│  ZMQ: 5559       │◄──┤
│                  │   │ Sends ZMQ
│  Input 0: Live   │   │ commands
│  Input 1: Offline│   │
└────────┬─────────┘   │
         │             │
         │ Persistent  │
         │ RTMP        │
         ▼             │
┌──────────────────┐   │
│  Kick.com (IVS)  │   │
└──────────────────┘   │
                       │
┌──────────────────┐   │
│  supervisor      │───┘
│  Python script   │
└──────────────────┘
```

## System Components

### 1. nginx-rtmp
- Receives incoming RTMP streams from your camera/OBS
- Provides XML stats endpoint for monitoring
- Lightweight, proven RTMP server

### 2. ffmpeg-relay
- Runs FFmpeg with dual inputs (live RTMP + offline MP4)
- Uses `streamselect` and `aselect` filters for switching
- Exposes ZMQ control socket on port 5559
- Maintains persistent connection to Kick

### 3. supervisor
- Python script that monitors nginx-rtmp stats
- Detects live stream status (external IP, video bitrate)
- Sends ZMQ commands to switch FFmpeg inputs
- Handles automatic failover and recovery

## Prerequisites

- Docker and Docker Compose installed
- An offline video file (MP4 format recommended)
- Kick.com streaming credentials (RTMPS URL and stream key)
- OBS or similar software for publishing RTMP stream

## Quick Start

### 1. Clone and Setup

```bash
git clone <your-repo-url>
cd relayer
```

### 2. Create Configuration

```bash
cp .env.example .env
```

Edit `.env` with your settings:

```bash
# Kick (IVS) Configuration
KICK_URL=rtmps://YOUR_INGEST_URL.global-contribute.live-video.net/app
KICK_KEY=sk_us-west-2_YOUR_STREAM_KEY

# Local RTMP Settings
RTMP_PORT=1936
RTMP_APP=live
RTMP_STREAM_NAME=mystream

# Offline Video (full path on host)
OFFLINE_VIDEO_PATH=/path/to/your/offline.mp4

# Video Encoding Settings
OUT_RES=1080
OUT_FPS=30
VID_BITRATE=3000k
MAX_BITRATE=3500k
BUFFER_SIZE=12000k

# Audio Encoding Settings
AUDIO_BITRATE=128k
AUDIO_SAMPLERATE=48000

# Supervisor Settings
POLL_INTERVAL=1
KILL_GRACE=3
CRASH_BACKOFF=2

# Logging
LOG_DIR=./logs
```

### 3. Prepare Offline Video

Make sure your offline video file exists at the path specified in `OFFLINE_VIDEO_PATH`.

### 4. Start the System

```bash
# Create log directory
mkdir -p logs

# Start all containers
docker-compose up -d

# View logs
docker-compose logs -f
```

### 5. Configure OBS

In OBS, configure your streaming settings:

**Server:** `rtmp://YOUR_VPS_IP:1936/live`  
**Stream Key:** `mystream`

## Configuration Details

### Video Settings

- `OUT_RES`: Output resolution height (720, 1080, etc.)
- `OUT_FPS`: Output framerate (30, 60)
- `VID_BITRATE`: Target video bitrate
- `MAX_BITRATE`: Maximum allowed bitrate
- `BUFFER_SIZE`: Buffer size for rate control

### Audio Settings

- `AUDIO_BITRATE`: Audio bitrate (typically 128k)
- `AUDIO_SAMPLERATE`: Audio sample rate (typically 48000)

### Supervisor Settings

- `POLL_INTERVAL`: How often to check stream status (seconds)
- `KILL_GRACE`: Grace period when stopping processes (seconds)
- `CRASH_BACKOFF`: Delay before restarting after crash (seconds)

## How It Works

### Stream Detection

The supervisor checks for live stream by examining nginx-rtmp stats:

1. **Video Bitrate Check** - Ensures video data is flowing (> 0)
2. **Publisher Check** - Verifies at least one client is publishing
3. **External IP Check** - Confirms publisher IP is NOT:
   - Loopback (127.x.x.x)
   - Private RFC1918 (10.x.x.x, 192.168.x.x, 172.16-31.x.x)

Only when ALL conditions are met does it switch to live mode.

### ZMQ Control Protocol

FFmpeg exposes a ZMQ socket that accepts text commands:

- `streamselect map 0` - Switch video to input 0 (live)
- `streamselect map 1` - Switch video to input 1 (offline)
- `aselect map 0` - Switch audio to input 0 (live)
- `aselect map 1` - Switch audio to input 1 (offline)

The supervisor sends these commands when state changes are detected.

### Silent Audio Injection

If the live stream lacks audio, FFmpeg automatically injects silent audio to maintain stream compatibility with Kick's requirements.

## Managing the System

### Start All Services

```bash
docker-compose up -d
```

### Stop All Services

```bash
docker-compose down
```

### Restart a Specific Service

```bash
docker-compose restart ffmpeg-relay
docker-compose restart supervisor
docker-compose restart nginx-rtmp
```

### View Logs

```bash
# All services
docker-compose logs -f

# Specific service
docker-compose logs -f supervisor
docker-compose logs -f ffmpeg-relay
docker-compose logs -f nginx-rtmp
```

### Check Service Status

```bash
docker-compose ps
```

## Troubleshooting

### Stream Not Switching to Live

1. Check supervisor logs: `docker-compose logs -f supervisor`
2. Verify OBS is publishing to correct URL/port
3. Check nginx-rtmp stats: `curl http://localhost:8080/rtmp_stat`
4. Ensure your source IP is not private/loopback

### FFmpeg Errors

1. Check FFmpeg logs: `docker-compose logs -f ffmpeg-relay`
2. Verify offline video file exists and is readable
3. Check Kick credentials in `.env`
4. Ensure FFmpeg has ZMQ support (included in linuxserver/ffmpeg)

### No Video on Kick

1. Verify stream key and URL are correct
2. Check FFmpeg is running: `docker-compose ps ffmpeg-relay`
3. Review FFmpeg logs for connection errors
4. Test with manual ffmpeg command outside Docker

### High CPU Usage

1. Lower resolution in `.env` (try 720p instead of 1080p)
2. Reduce FPS (try 30 instead of 60)
3. Lower video bitrate
4. Use faster x264 preset (already using veryfast)

## Monitoring

### Check nginx-rtmp Stats

```bash
curl http://localhost:8080/rtmp_stat
```

### Check Container Health

```bash
docker-compose ps
docker stats relayer-nginx-rtmp relayer-ffmpeg relayer-supervisor
```

### View Real-time Logs

```bash
# Supervisor activity
docker-compose logs -f supervisor | grep -E "LIVE|OFFLINE|Switch"

# FFmpeg errors
docker-compose logs -f ffmpeg-relay | grep -i error
```

## Project Structure

```
relayer/
├── .env                    # Your configuration (git-ignored)
├── .env.example            # Template configuration
├── docker-compose.yml      # Service orchestration
├── logs/                   # Log output directory
│   ├── ffmpeg.log
│   └── supervisor.log
├── nginx-rtmp/
│   ├── Dockerfile
│   └── nginx.conf
├── ffmpeg-relay/
│   ├── Dockerfile
│   └── entrypoint.sh
├── supervisor/
│   ├── Dockerfile
│   ├── requirements.txt
│   └── supervisor.py
└── README.md
```

## Advanced Usage

### Manual Control via ZMQ

You can manually send commands to FFmpeg:

```bash
# Switch to live
echo "streamselect map 0" | nc localhost 5559
echo "aselect map 0" | nc localhost 5559

# Switch to offline
echo "streamselect map 1" | nc localhost 5559
echo "aselect map 1" | nc localhost 5559
```

### Custom nginx-rtmp Configuration

Edit `nginx-rtmp/nginx.conf` for custom RTMP settings, then:

```bash
docker-compose up -d --build nginx-rtmp
```

### Adjust Encoding on the Fly

Modify `.env`, then restart FFmpeg:

```bash
docker-compose up -d --force-recreate ffmpeg-relay
```
## Running Multiple Instances

You can run multiple instances of this relay system on the same machine to stream to different platforms simultaneously (e.g., Kick, YouTube, Twitch). Each instance needs unique ports and a unique project name.

### Setup for Multiple Instances

#### Method 1: Using Separate .env Files

1. **Create separate .env files for each instance:**

```bash
# Use the provided examples as templates
cp .env.instance1 .env.relay1
cp .env.instance2 .env.relay2
cp .env.instance3 .env.relay3
```

2. **Edit each file with unique settings:**

```bash
# .env.relay1 (Kick)
COMPOSE_PROJECT_NAME=relay1
RTMP_PORT=1936
HTTP_STATS_PORT=8080
KICK_URL=rtmps://your-kick-ingest.net/app
KICK_KEY=your_kick_key
OFFLINE_VIDEO_PATH=/opt/offline-kick.mp4
LOG_DIR=./logs/relay1

# .env.relay2 (YouTube)
COMPOSE_PROJECT_NAME=relay2
RTMP_PORT=1937
HTTP_STATS_PORT=8081
KICK_URL=rtmp://a.rtmp.youtube.com/live2
KICK_KEY=your_youtube_key
OFFLINE_VIDEO_PATH=/opt/offline-youtube.mp4
LOG_DIR=./logs/relay2

# .env.relay3 (Twitch)
COMPOSE_PROJECT_NAME=relay3
RTMP_PORT=1938
HTTP_STATS_PORT=8082
KICK_URL=rtmp://live.twitch.tv/app
KICK_KEY=your_twitch_key
OFFLINE_VIDEO_PATH=/opt/offline-twitch.mp4
LOG_DIR=./logs/relay3
```

3. **Start each instance:**

```bash
# Create log directories
mkdir -p logs/relay1 logs/relay2 logs/relay3

# Start instance 1 (Kick)
docker-compose --env-file .env.relay1 up -d

# Start instance 2 (YouTube)
docker-compose --env-file .env.relay2 up -d

# Start instance 3 (Twitch)
docker-compose --env-file .env.relay3 up -d
```

#### Method 2: Using Environment Variables

You can also override settings directly without separate .env files:

```bash
# Start instance 1
COMPOSE_PROJECT_NAME=relay1 RTMP_PORT=1936 HTTP_STATS_PORT=8080 docker-compose up -d

# Start instance 2
COMPOSE_PROJECT_NAME=relay2 RTMP_PORT=1937 HTTP_STATS_PORT=8081 docker-compose up -d

# Start instance 3
COMPOSE_PROJECT_NAME=relay3 RTMP_PORT=1938 HTTP_STATS_PORT=8082 docker-compose up -d
```

### Managing Multiple Instances

#### View Status of All Instances

```bash
# List all containers
docker ps --filter "name=relay"

# Or use docker-compose for specific instance
docker-compose --env-file .env.relay1 ps
docker-compose --env-file .env.relay2 ps
docker-compose --env-file .env.relay3 ps
```

#### View Logs from Specific Instance

```bash
# Instance 1
docker-compose --env-file .env.relay1 logs -f

# Instance 2 supervisor only
docker-compose --env-file .env.relay2 logs -f supervisor

# All relay containers
docker logs -f relay1-ffmpeg-relay-1
docker logs -f relay2-supervisor-1
```

#### Stop Specific Instance

```bash
# Stop instance 1
docker-compose --env-file .env.relay1 down

# Stop instance 2
docker-compose --env-file .env.relay2 down

# Stop all relay instances
docker-compose --env-file .env.relay1 down
docker-compose --env-file .env.relay2 down
docker-compose --env-file .env.relay3 down
```

#### Restart Specific Service in Instance

```bash
# Restart FFmpeg in instance 1
docker-compose --env-file .env.relay1 restart ffmpeg-relay

# Restart supervisor in instance 2
docker-compose --env-file .env.relay2 restart supervisor
```

### Port Requirements

When running multiple instances, ensure each has unique ports:

| Instance | RTMP Port | HTTP Stats Port | OBS Stream URL |
|----------|-----------|-----------------|----------------|
| relay1   | 1936      | 8080           | rtmp://IP:1936/live/mystream |
| relay2   | 1937      | 8081           | rtmp://IP:1937/live/mystream |
| relay3   | 1938      | 8082           | rtmp://IP:1938/live/mystream |

### Container Naming

Each instance will have uniquely named containers based on `COMPOSE_PROJECT_NAME`:

```
relay1-nginx-rtmp-1
relay1-ffmpeg-relay-1
relay1-supervisor-1

relay2-nginx-rtmp-1
relay2-ffmpeg-relay-1
relay2-supervisor-1

relay3-nginx-rtmp-1
relay3-ffmpeg-relay-1
relay3-supervisor-1
```

### Network Isolation

Each instance creates its own Docker network:

- `relay1_relayer-network`
- `relay2_relayer-network`
- `relay3_relayer-network`

This ensures complete isolation between instances.

### OBS Configuration for Multiple Streams

To stream to all instances simultaneously from OBS:

1. Use the built-in multi-stream plugin or Aitum Multistream
2. Or publish once and use OBS Advanced Scene Switcher with multiple stream outputs
3. Or run multiple OBS instances (resource intensive)

**Recommended:** Configure OBS with multiple stream outputs:

- Output 1: `rtmp://localhost:1936/live/mystream` (Kick)
- Output 2: `rtmp://localhost:1937/live/mystream` (YouTube)
- Output 3: `rtmp://localhost:1938/live/mystream` (Twitch)

### Resource Considerations

Each instance runs 3 containers and 1 FFmpeg process. For 3 instances:

- **Containers:** 9 total (3 per instance)
- **FFmpeg Processes:** 3 (most resource-intensive)
- **RAM:** ~2GB per instance = ~6GB total
- **CPU:** Depends on resolution/bitrate (1080p@30fps uses ~50% of 1 core per instance)
- **Upload Bandwidth:** Sum of all bitrates (3Mbps × 3 = 9Mbps minimum)

### Troubleshooting Multiple Instances

#### Port Conflicts

If you get "port already in use" errors:

```bash
# Check what's using the port
sudo lsof -i :1936
sudo lsof -i :8080

# Ensure each instance has unique ports in its .env file
```

#### Container Name Conflicts

If you get "container name already in use":

```bash
# Remove old containers
docker-compose down
docker-compose --env-file .env.relay1 down
docker-compose --env-file .env.relay2 down

# Ensure COMPOSE_PROJECT_NAME is unique in each .env file
```

#### Checking Stats from Multiple Instances

```bash
# Instance 1 stats
curl http://localhost:8080/rtmp_stat

# Instance 2 stats
curl http://localhost:8081/rtmp_stat

# Instance 3 stats
curl http://localhost:8082/rtmp_stat
```


## Security Considerations

- Keep `.env` file secure and never commit to git
- Consider using Docker secrets for production
- Restrict RTMP port (1936) to known IPs if possible
- Use firewall rules to protect stats endpoint (8080)

## Performance Tips

1. **Use SSD storage** for better I/O when reading offline video
2. **Allocate sufficient RAM** - at least 2GB for smooth operation
3. **Monitor network bandwidth** - ensure sufficient upload speed
4. **Test offline video encoding** - pre-encode at target resolution/bitrate
5. **Use wired connection** - avoid WiFi for production streaming

## License

MIT License - see LICENSE file for details

## Support

For issues, questions, or contributions, please open an issue on GitHub.