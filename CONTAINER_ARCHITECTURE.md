# Container Architecture Diagram

## Overview
The RTMP Stream Relay System consists of 10 Docker containers working together to manage, switch, and relay video streams.

## Container Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        HOST SYSTEM                               │
│  ┌──────────────┐              ┌──────────────┐                 │
│  │ brb.mp4  │              │ brb2.mp4 │                 │
│  │ (video loop) │              │ (dev camera) │                 │
│  └──────┬───────┘              └──────┬───────┘                 │
└─────────┼──────────────────────────────┼──────────────────────────┘
          │                              │
          │                              │      ┌──────────────────┐
          │                              │      │ External SRT     │
          │                              │      │ Source           │
          │                              │      └────────┬─────────┘
          │                              │               │ SRT Port 1937
┌─────────▼──────────────────────────────▼───────────────▼──────────┐
│                     DOCKER NETWORK (rtmp-network)                 │
│                                                                    │
│  ┌────────────────┐  ┌──────────────────┐  ┌─────────────────┐  │
│  │ ffmpeg-brb │  │ ffmpeg-cam-dev   │  │  ffmpeg-srt     │  │
│  │                │  │ (manual profile) │  │                 │  │
│  │ Loops brb  │  │ Simulates camera │  │ SRT listener    │  │
│  │ video to RTMP  │  │ with video file  │  │ relays to RTMP  │  │
│  └────────┬───────┘  └────────┬─────────┘  └────────┬────────┘  │
│           │                   │                      │            │
│           │ /live/brb      │ /live/cam-raw      │ /live/cam-raw│
│           │                   │                      │            │
│           │            ┌──────┴──────────────────────┘            │
│           │            │                                          │
│           │      ┌─────▼─────────────┐                           │
│           │      │ ffmpeg-cam-normalized  │                           │
│           │      │ Normalizes camera │                           │
│           │      │ for GStreamer     │                           │
│           │      └─────┬─────────────┘                           │
│           │            │ /live/cam                                │
│           └───────┬────┴───────────────┐                         │
│                   │                    │                          │
│            ┌──────▼────────┐           │                          │
│            │  nginx-rtmp   │◄──────────┘                          │
│            │               │◄─────────┐                           │
│            │ Port 1936     │          │ /live/program             │
│            │ Stats: 8080   │          │                           │
│            └───┬───────┬───┘          │                           │
│                │       │              │                           │
│   /live/brb    │       │ /live/cam    │                           │
│                │       │              │                           │
│         ┌──────▼───────▼──────┐       │                           │
│         │  muxer    │───────┘                           │
│                 │                     │                           │
│                 │ GStreamer pipeline  │                           │
│                 │ API Port: 8088      │                           │
│                 │ Switches sources    │                           │
│                 └──────┬──────────────┘                           │
│                        │                                          │
│              Reads /live/program                                  │
│                        │                                          │
│                 ┌──────▼──────────┐                               │
│                 │  ffmpeg-kick    │                               │
│                 │                 │                               │
│                 │ Relays to Kick  │                               │
│                 │ streaming       │                               │
│                 └─────────────────┘                               │
│                                                                    │
│  ┌──────────────────────────────────────────────────────┐        │
│  │            MONITORING & CONTROL LAYER                 │        │
│  │                                                        │        │
│  │  ┌────────────────────┐     ┌──────────────────────┐ │        │
│  │  │ stream-auto-       │     │ stream-controller    │ │        │
│  │  │ switcher           │     │                      │ │        │
│  │  │                    │     │ Port: 8089           │ │        │
│  │  │ (auto profile)     │     │ Docker socket access │ │        │
│  │  │                    │     │ Container lifecycle  │ │        │
│  │  │ Monitors nginx     │     │ management           │ │        │
│  │  │ stats, controls    │     └──────────┬───────────┘ │        │
│  │  │ switcher API       │                │             │        │
│  │  └────────────────────┘                │             │        │
│  │                                         │             │        │
│  │  ┌──────────────────────────────────────▼───────┐    │        │
│  │  │         stream-dashboard                     │    │        │
│  │  │                                              │    │        │
│  │  │  Port: 3000 (HTTP + WebSocket)              │    │        │
│  │  │  Vue.js UI with real-time monitoring        │    │        │
│  │  │  Controls: Start/Stop/Restart containers    │    │        │
│  │  └──────────────────────────────────────────────┘    │        │
│  └──────────────────────────────────────────────────────┘        │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
                              │
                              │ RTMPS
                              ▼
                    ┌──────────────────┐
                    │   Kick Server    │
                    │ (External)       │
                    └──────────────────┘
```

## Container Details

### 1. **nginx-rtmp** (Core Hub)
- **Purpose**: Central RTMP relay server that receives, stores, and distributes video streams
- **Technology**: NGINX with RTMP module
- **Ports**:
  - `1936`: RTMP streaming port
  - `8080`: HTTP statistics and health endpoint
- **Streams**:
  - `/live/brb`: Receives brb video loop
  - `/live/cam-raw`: Receives raw camera input (Moblin, SRT, etc.)
  - `/live/cam`: Receives normalized camera feed (from ffmpeg-cam-normalized)
  - `/live/program`: Receives final output from muxer
- **Key Features**: 
  - Stream statistics via XML endpoint
  - Health monitoring
  - 10-second buffer for smooth playback

---

### 2. **ffmpeg-brb** (Always Active)
- **Purpose**: Continuously streams a pre-recorded video file in a loop
- **Technology**: FFmpeg
- **Input**: `brb.mp4` from host filesystem
- **Output**: `rtmp://nginx-rtmp:1936/live/brb`
- **Profile**: All (runs in both production and dev mode)
- **Use Case**: Fallback content when camera is unavailable

---

### 3. **ffmpeg-cam-dev** (Manual Service)
- **Purpose**: Simulates a camera feed using a second video file
- **Technology**: FFmpeg
- **Input**: `brb2.mp4` from host filesystem
- **Output**: `rtmp://nginx-rtmp:1936/live/cam`
- **Profile**: `manual` (requires explicit start)
- **Use Case**: Testing camera switching logic without real camera hardware

---

### 4. **ffmpeg-srt** (SRT Bridge)
- **Purpose**: Accepts external SRT streams and relays them to the internal RTMP infrastructure
- **Technology**: FFmpeg with SRT protocol support
- **Input**: `srt://0.0.0.0:9000?mode=listener` (exposed on host port 1937)
- **Output**: `rtmp://nginx-rtmp:1936/live/cam-raw`
- **Port**: `1937` (configurable via `SRT_PORT` environment variable)
- **Profile**: All (runs by default)
- **Key Features**:
  - Zero-latency stream copy (no re-encoding)
  - SRT listener mode for accepting external connections
  - Outputs to cam-raw for normalization
- **Use Case**: Receiving live video feeds from external SRT encoders, cameras, or OBS Studio

---

### 5. **ffmpeg-cam-normalized** (Camera Normalizer)
- **Purpose**: Normalizes raw camera streams to GStreamer-compatible format
- **Technology**: FFmpeg with H.264 transcoding
- **Input**: `rtmp://nginx-rtmp:1936/live/cam-raw`
- **Output**: `rtmp://nginx-rtmp:1936/live/cam`
- **Profile**: All (runs by default)
- **Key Features**:
  - Transcodes to GStreamer-compatible H.264 (Main profile, no B-frames)
  - Handles mobile hardware encoder quirks (Moblin, iOS, Android)
  - Auto-reconnects when input stream disconnects
  - Configurable bitrate, framerate, and quality settings
  - Infinite retry loop with configurable delay
  - Health monitoring via process check
- **Environment Variables**:
  - `VIDEO_BITRATE`: Video bitrate in kbps (default: 3000)
  - `AUDIO_BITRATE`: Audio bitrate in kbps (default: 128)
  - `FRAMERATE`: Output framerate (default: 30)
  - `GOP_SIZE`: Keyframe interval in frames (default: 60 = 2s)
  - `MAX_RETRIES`: Maximum retry attempts, 0=infinite (default: 0)
  - `RETRY_DELAY`: Seconds between retries (default: 5)
- **Use Case**: Fixes black screen/no audio issues with Moblin and other mobile streaming apps
- **Problem Solved**: GStreamer's uridecodebin struggles with H.264 High profile and B-frames from mobile hardware encoders

---

### 6. **muxer** (Scene Switcher)
- **Purpose**: Dynamically switches between multiple input streams and outputs a single program stream
- **Technology**: GStreamer with Python control API
- **API Port**: `8088`
- **Inputs**: 
  - `rtmp://nginx-rtmp:1936/live/brb`
  - `rtmp://nginx-rtmp:1936/live/cam`
- **Output**: `rtmp://nginx-rtmp:1936/live/program`
- **Key Features**:
  - RESTful API for manual switching (`/switch?src=brb` or `src=cam`)
  - Seamless transitions using input-selector element
  - Re-encodes to H.264/AAC at 1080p30, 3000 kbps

---

### 7. **stream-auto-switcher** (Automated Quality Manager)
- **Purpose**: Monitors stream quality and automatically switches between camera and brb content
- **Technology**: Python with XML parsing
- **Profile**: `auto` (optional, runs with `--profile auto`)
- **Monitoring**: Polls nginx-rtmp stats every 0.5 seconds
- **Decision Logic**:
  - Switches TO camera when stream is stable with sufficient bitrate (>300 kbps)
  - Switches FROM camera when quality degrades or stream disappears
  - Implements grace periods for stability (2s) and degradation tolerance (3s)
- **Key Features**:
  - Bitrate threshold monitoring
  - State machine for intelligent switching
  - Prevents rapid switching (debouncing)

---

### 8. **ffmpeg-kick** (External Relay)
- **Purpose**: Relays the final program stream to Kick streaming platform
- **Technology**: FFmpeg with RTMPS
- **Input**: `rtmp://nginx-rtmp:1936/live/program`
- **Output**: Kick RTMPS server (configured via environment variables)
- **Key Features**:
  - 5-second RTMP buffer for smooth playback
  - Handles Kick-specific streaming requirements
  - Verbose logging for diagnostics

---

### 9. **stream-controller** (Container Manager)
- **Purpose**: Provides REST API for container lifecycle management
- **Technology**: Python with Docker SDK
- **API Port**: `8089`
- **Access**: Docker socket (`/var/run/docker.sock`)
- **Operations**:
  - Start/Stop/Restart any container
  - Query container status
  - List all managed containers
- **Endpoints**:
  - `POST /container/{name}/start`
  - `POST /container/{name}/stop`
  - `POST /container/{name}/restart`
  - `GET /container/{name}/status`
  - `GET /containers`

---

### 10. **stream-dashboard** (Web Interface)
- **Purpose**: Provides web-based monitoring and control interface
- **Technology**: Vue.js 3 frontend + Express.js backend + WebSocket
- **Port**: `3000` (HTTP + WebSocket)
- **Features**:
  - **Real-time Monitoring**:
    - System metrics (CPU, memory, load)
    - RTMP stream statistics and bandwidth
    - Current active scene indicator
  - **Container Control**:
    - Visual start/stop/restart buttons
    - Container status indicators
  - **Live Updates**: WebSocket updates every 2 seconds
- **Architecture**: 
  - Frontend: Vue.js single-page application
  - Backend: Express.js aggregating data from multiple services
  - Services layer: Controller, metrics, RTMP parser

---

## Data Flow

### Video Stream Flow:
1. `brb.mp4` → **ffmpeg-brb** → nginx-rtmp `/live/brb`
2. `brb2.mp4` → **ffmpeg-cam-dev** → nginx-rtmp `/live/cam-raw` (manual)
3. External SRT stream → **ffmpeg-srt** → nginx-rtmp `/live/cam-raw`
4. External Moblin/RTMP → nginx-rtmp `/live/cam-raw`
5. nginx-rtmp `/live/cam-raw` → **ffmpeg-cam-normalized** → nginx-rtmp `/live/cam`
6. nginx-rtmp streams (`/live/brb`, `/live/cam`) → **muxer** → nginx-rtmp `/live/program`
7. nginx-rtmp `/live/program` → **ffmpeg-kick** → Kick streaming platform

### Control Flow:
1. **stream-auto-switcher** monitors nginx-rtmp stats
2. **stream-auto-switcher** sends switch commands to **muxer** API
3. **stream-dashboard** queries **stream-controller** for container status
4. **stream-dashboard** queries nginx-rtmp for stream statistics
5. **stream-dashboard** queries **muxer** for current scene

### Management Flow:
1. User accesses **stream-dashboard** web UI (port 3000)
2. Dashboard sends control commands to **stream-controller** API (port 8089)
3. **stream-controller** manages containers via Docker socket
4. Dashboard receives real-time updates via WebSocket

---

## Network Configuration

- **Network Name**: `rtmp-network`
- **Type**: Docker bridge network
- **Internal Communication**: All containers communicate using service names as hostnames
- **External Access**: Only specific ports are exposed to the host:
  - `1937`: SRT input (ffmpeg-srt listener)
  - `1936`: RTMP streaming
  - `8080`: NGINX stats
  - `8088`: Stream switcher API
  - `8089`: Container controller API
  - `3000`: Web dashboard

---

## Startup Dependencies

```
nginx-rtmp (healthy)
    ├── ffmpeg-brb
    ├── ffmpeg-cam-dev (manual profile)
    ├── ffmpeg-srt
    ├── ffmpeg-cam-normalized
    └── muxer (after ffmpeg-brb)
            ├── stream-auto-switcher (healthy, auto profile)
            └── ffmpeg-kick (manual profile, healthy)

stream-controller (independent)
    └── stream-dashboard (after nginx-rtmp and stream-controller healthy)
```

---

## Profiles

- **Default**: nginx-rtmp, ffmpeg-brb, ffmpeg-srt, ffmpeg-cam-normalized, muxer, stream-controller, stream-dashboard
- **manual**: ffmpeg-cam-dev (camera simulation), ffmpeg-kick (Kick streaming)
- **auto**: Adds stream-auto-switcher for automatic quality management

Profiles can be combined: `docker compose --profile manual --profile auto up -d`

---

## Health Checks

All critical services implement health endpoints:
- **nginx-rtmp**: `http://localhost:8080/health`
- **muxer**: `http://localhost:8088/health`
- **stream-controller**: `http://localhost:8089/health`
- **stream-dashboard**: `http://localhost:3000/api/health`

Health checks ensure proper startup ordering and service reliability.