# TSDuck MPEG-TS Multiplexer with Automatic Failover

A high-availability MPEG-TS multiplexer built with TSDuck that provides automatic failover between live SRT input and a fallback video stream, outputting a continuous RTMP stream with seamless timestamp management.

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Features](#features)
- [Requirements](#requirements)
- [Project Structure](#project-structure)
- [Installation](#installation)
- [Configuration](#configuration)
- [Usage](#usage)
- [How It Works](#how-it-works)
- [Troubleshooting](#troubleshooting)
- [Development](#development)

## Overview

This system consists of four Docker containers:

1. **Multiplexer** - C++ application using TSDuck for TS packet processing
2. **ffmpeg-srt-live** - Receives SRT input and converts to UDP TS
3. **ffmpeg-fallback** - Loops a fallback MPEG-TS file as UDP TS
4. **nginx-rtmp** - RTMP/HLS server for stream distribution and monitoring

The multiplexer intelligently switches between live and fallback sources while maintaining continuous output with monotonic timestamps. The integrated nginx-rtmp server provides both RTMP and HLS playback capabilities with real-time statistics monitoring.

## Architecture

```
┌─────────────────┐                           ┌──────────────────┐
│  SRT Input      │                           │  Clients         │
│  Port 1937      │                           ├──────────────────┤
└────────┬────────┘                           │ VLC/FFplay       │
         │                                    │ Web Browsers     │
         v                                    │ Mobile Apps      │
┌─────────────────────┐                       └────────┬─────────┘
│ ffmpeg-srt-live     │                                │
│ SRT → UDP TS:10000  │                                │
└──────────┬──────────┘                                │
           │                                           │
           v                                           │
┌──────────────────────┐      ┌────────────────────┐  │
│   Multiplexer        │      │ ffmpeg-fallback    │  │
│   - Timestamp Mgmt   │◄─────│ TS → UDP TS:10001  │  │
│   - PID Remapping    │      └────────────────────┘  │
│   - Mode Switching   │                              │
└──────────┬───────────┘                              │
           │                                           │
           v                                           v
┌──────────────────────┐                    ┌─────────────────────┐
│   nginx-rtmp Server  │◄───────────────────│  Port 1935 (RTMP)   │
│   - RTMP Publishing  │                    │  Port 8080 (HLS)    │
│   - HLS Streaming    │                    │  /stat (Monitor)    │
│   - Statistics       │                    └─────────────────────┘
└──────────────────────┘
```

## Features

- **Automatic Failover**: Switches to fallback if live TS stalls for >2 seconds (configurable)
- **Seamless Switching**: Maintains monotonic PTS/DTS/PCR timestamps during source transitions
- **PID Remapping**: Normalizes fallback PIDs to match live stream
- **Continuity Counter Management**: Ensures valid TS packet continuity
- **Thread-Safe Design**: Dedicated threads for UDP receivers and main processing
- **Comprehensive Logging**: Detailed logging of mode switches, timestamps, and stats
- **Integrated RTMP/HLS Server**: Built-in nginx-rtmp server for stream distribution
- **Real-time Statistics**: Web-based monitoring dashboard at `/stat` endpoint
- **Multi-Protocol Support**: RTMP for low latency, HLS for browser compatibility
- **Docker-Based**: Complete containerized deployment with docker-compose

## Requirements

### For Docker Deployment (Recommended)

- Docker 20.10+
- Docker Compose 2.0+
- Environment variables configured (SHARED_FOLDER)

### For Local Development

- Ubuntu 22.04 or compatible Linux
- CMake 3.15+
- GCC 9+ or Clang 10+
- TSDuck library
- yaml-cpp library
- FFmpeg

## Project Structure

```
tsduck-multiplexer/
├── CMakeLists.txt              # Build configuration
├── config.yaml                 # Runtime configuration
├── docker-compose.yml          # Docker orchestration
├── nginx.conf                  # nginx-rtmp server config
├── ARCHITECTURE.md             # Detailed architecture docs
├── README.md                   # This file
├── docker/
│   ├── Dockerfile.multiplexer  # Multiplexer container
│   ├── Dockerfile.ffmpeg-fallback  # Fallback container
│   ├── generate-fallback.sh   # Generates default BRB video
│   └── convert-fallback.sh    # Converts MP4 to MPEG-TS
├── src/
│   ├── main.cpp               # Entry point
│   ├── Config.{h,cpp}         # Configuration parser
│   ├── TSPacketQueue.{h,cpp}  # Thread-safe packet queue
│   ├── UDPReceiver.{h,cpp}    # UDP TS receiver
│   ├── TSAnalyzer.{h,cpp}     # PMT/PID extraction
│   ├── TimestampManager.{h,cpp}  # Timestamp control
│   ├── PIDMapper.{h,cpp}      # PID remapping
│   ├── StreamSwitcher.{h,cpp} # LIVE/FALLBACK logic
│   ├── RTMPOutput.{h,cpp}     # FFmpeg RTMP output
│   └── Multiplexer.{h,cpp}    # Main orchestrator
└── shared/                     # Shared folder (mounted to containers)
    └── fallback.ts            # Auto-generated or custom fallback video
```

## Installation

### Docker Deployment (Recommended)

1. **Clone the repository:**
   ```bash
   git clone <repository-url>
   cd tsduck-multiplexer
   ```

2. **Configure environment:**
   ```bash
   # Create .env file with your shared folder path
   echo "SHARED_FOLDER=/path/to/your/shared/folder" >> .env
   ```

3. **Build and start:**
   ```bash
   docker-compose build
   docker-compose up -d
   ```
   
   The system includes an integrated nginx-rtmp server, so no external RTMP server is required for testing.
   
   **Note:** On first startup, if no `fallback.ts` exists in the shared folder, the ffmpeg-fallback container will automatically generate a default fallback video (black screen with "BRB..." text). This takes ~10-15 seconds.

4. **Optional: Custom fallback video**
   
   If you want to use your own fallback video instead of the auto-generated one:
   ```bash
   # Convert your video to MPEG-TS format and place in shared folder
   ffmpeg -i /path/to/your/video.mp4 \
     -c:v libx264 -preset fast -crf 23 \
     -g 30 -keyint_min 30 -sc_threshold 0 \
     -c:a aac -b:a 128k \
     -bsf:v h264_mp4toannexb \
     -f mpegts -mpegts_flags +resend_headers \
     $SHARED_FOLDER/fallback.ts
   ```

### Local Build (Development)

1. **Install dependencies:**
   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential cmake git pkg-config \
       libssl-dev libcurl4-openssl-dev libyaml-cpp-dev ffmpeg
   ```

2. **Build and install TSDuck:**
   ```bash
   git clone https://github.com/tsduck/tsduck.git
   cd tsduck
   make NOTELETEXT=1 NOSRT=1 NORIST=1 NODTAPI=1
   sudo make install NOTELETEXT=1 NOSRT=1 NORIST=1 NODTAPI=1
   sudo ldconfig
   cd ..
   ```

3. **Build the multiplexer:**
   ```bash
   mkdir build && cd build
   cmake ..
   make -j$(nproc)
   ```

4. **Run:**
   ```bash
   ./ts-multiplexer ../config.yaml
   ```

## Configuration

Edit [`config.yaml`](config.yaml):

```yaml
# UDP port for live TS input
live_udp_port: 10000

# UDP port for fallback TS input
fallback_udp_port: 10001

# RTMP output URL (internal Docker network)
rtmp_url: "rtmp://nginx-rtmp/live/stream"

# Gap timeout before switching to fallback (milliseconds)
max_live_gap_ms: 2000

# Logging level
log_level: "INFO"
```

**Default Configuration**: The system uses the integrated nginx-rtmp server at `rtmp://nginx-rtmp/live/stream`. For external RTMP servers, update the URL accordingly.

## Usage

### Starting the System

```bash
# Start all containers
docker-compose up -d

# View logs
docker-compose logs -f multiplexer

# View all container logs
docker-compose logs -f
```

### Sending Live Video

Send an SRT stream to the system:

```bash
# Using FFmpeg
ffmpeg -re -i input.mp4 \
  -c copy \
  -f mpegts "srt://localhost:1937?mode=caller"

# Using OBS Studio
# Set output to: srt://your-server-ip:1937
# Mode: Caller
```

### Watching the Stream

**RTMP Playback** (Low latency, ~2-5 seconds):
```bash
# Using VLC
vlc rtmp://localhost:1935/live/stream

# Using ffplay
ffplay rtmp://localhost:1935/live/stream

# Using mpv
mpv rtmp://localhost:1935/live/stream
```

**HLS Playback** (Browser compatible, ~10-15 seconds latency):
```bash
# Direct URL
http://localhost:8080/hls/stream.m3u8

# Using VLC
vlc http://localhost:8080/hls/stream.m3u8

# In browser (Chrome, Firefox, Safari)
# Use an HLS player like Video.js or hls.js
```

**Statistics Dashboard**:
```bash
# View real-time statistics in browser
http://localhost:8080/stat

# Get statistics as XML (for automation)
http://localhost:8080/stat.xml
```

The statistics page shows:
- Active streams and viewers
- Bitrate (in/out)
- Video codec, resolution, framerate
- Audio codec, sample rate
- Stream uptime
- Bytes transmitted

### Monitoring

The multiplexer logs show:

- Current mode (LIVE/FALLBACK)
- Mode transitions with timing
- Packets processed and queue sizes
- PID information from both streams
- Timestamp offsets

Example log output:
```
[Multiplexer] Configuration loaded successfully
[Live] Started on UDP port 10000
[Fallback] Started on UDP port 10001
[Multiplexer] Live stream:
  Video PID: 256
  Audio PID: 257
[StreamSwitcher] LIVE → FALLBACK (gap=2100ms)
[Multiplexer] Switched to FALLBACK mode
[StreamSwitcher] FALLBACK → LIVE (consecutive packets=10)
[Multiplexer] Switched to LIVE mode
```

### Stopping the System

```bash
# Stop all containers
docker-compose down

# Stop and remove volumes
docker-compose down -v
```

## How It Works

### Source Selection

1. **Initial State**: Starts in LIVE mode
2. **Fallback Trigger**: If no live packets for >2 seconds → switch to FALLBACK
3. **Live Resume**: When 10 consecutive live packets arrive → switch back to LIVE

### Timestamp Management

The system maintains timeline continuity by:

1. **Extracting** original PTS/DTS/PCR from each packet
2. **Calculating** source-specific offsets
3. **Adjusting** timestamps: `output_pts = input_pts + offset`
4. **Enforcing** monotonic increase (no backward jumps)
5. **Rewriting** PES headers and PCR fields

When switching sources:
```
last_output_pts = 5000 (from live)
first_fallback_pts = 100

Calculate offset:
fallback_offset = 5000 - 100 + frame_duration = 4940

Output timeline continues:
output_pts = 100 + 4940 = 5040 ✓
```

### PID Remapping

Fallback stream PIDs are remapped to match live stream PIDs:
```
Live:     Video PID=256, Audio PID=257
Fallback: Video PID=300, Audio PID=301

Mapping:  300→256, 301→257
```

### Continuity Counters

Each output PID maintains a continuity counter (0-15) that increments for each packet, ensuring valid TS stream structure.

## Troubleshooting

### No RTMP Output

**Check FFmpeg process:**
```bash
docker exec ts-multiplexer ps aux | grep ffmpeg
```

**Verify RTMP URL:**
- Ensure `rtmp_url` in config.yaml is correct
- Test with: `ffplay rtmp://your-server/live/streamKey`

### No Live Input Detected

**Check SRT connection:**
```bash
# View ffmpeg-srt-live logs
docker-compose logs ffmpeg-srt-live
```

**Test SRT connectivity:**
```bash
# Simple test
ffmpeg -re -i test.mp4 -c copy -f mpegts "srt://localhost:1937?mode=caller"
```

### Stuck in Fallback Mode

**Verify live stream:**
- Check that live TS packets are arriving on UDP 10000
- Ensure live stream has valid PMT/PAT

**Check logs:**
```bash
docker-compose logs -f multiplexer | grep "Live"
```

### Build Errors

**TSDuck not found:**
```bash
# Verify TSDuck installation
pkg-config --modversion tsduck
ldconfig -p | grep tsduck
```

**yaml-cpp missing:**
```bash
sudo apt-get install libyaml-cpp-dev
```

## Development

### Running Tests

(Add your test framework here)

### Code Structure

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for detailed component documentation.

### Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

### Debugging

Enable verbose logging:
```yaml
log_level: "DEBUG"
```

Run multiplexer with gdb:
```bash
docker exec -it ts-multiplexer bash
gdb /usr/local/bin/ts-multiplexer
```

## Performance

- **Latency**: Typically <100ms end-to-end
- **CPU Usage**: ~5-10% on modern CPU (1 core)
- **Memory**: ~50MB for multiplexer + FFmpeg processes
- **Throughput**: Supports up to 20 Mbps TS streams

## License

[Specify your license here]

## Acknowledgments

- [TSDuck](https://tsduck.io/) - MPEG transport stream toolkit
- [FFmpeg](https://ffmpeg.org/) - Multimedia framework
- [yaml-cpp](https://github.com/jbeder/yaml-cpp) - YAML parser

## Support

For issues and questions:
- GitHub Issues: [Link to your repository]
- Documentation: See `ARCHITECTURE.md` for technical details# latestrelayer
- Test commit
- Another test
