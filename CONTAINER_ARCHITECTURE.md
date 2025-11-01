# Container Architecture Diagram

## Overview
The RTMP Stream Relay System consists of 8 Docker containers working together to manage, switch, and relay video streams.

## Container Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        HOST SYSTEM                               │
│  ┌──────────────┐              ┌──────────────┐                 │
│  │ offline.mp4  │              │ offline2.mp4 │                 │
│  │ (video loop) │              │ (dev camera) │                 │
│  └──────┬───────┘              └──────┬───────┘                 │
└─────────┼──────────────────────────────┼──────────────────────────┘
          │                              │
          │                              │
┌─────────▼──────────────────────────────▼──────────────────────────┐
│                     DOCKER NETWORK (rtmp-network)                 │
│                                                                    │
│  ┌────────────────┐              ┌──────────────────┐            │
│  │ ffmpeg-offline │              │ ffmpeg-dev-cam   │            │
│  │                │              │ (dev profile)    │            │
│  │ Loops offline  │              │ Simulates camera │            │
│  │ video to RTMP  │              │ with video file  │            │
│  └────────┬───────┘              └────────┬─────────┘            │
│           │                               │                       │
│           │ /live/offline                 │ /live/cam            │
│           │                               │                       │
│           └───────────────┬───────────────┘                       │
│                           │                                       │
│                    ┌──────▼────────┐                              │
│                    │  nginx-rtmp   │◄─────────┐                  │
│                    │               │          │                  │
│                    │ Port 1936     │          │ /live/program    │
│                    │ Stats: 8080   │          │                  │
│                    └───┬───────┬───┘          │                  │
│                        │       │              │                  │
│         /live/offline  │       │ /live/cam    │                  │
│         /live/cam      │       │              │                  │
│                        │       │              │                  │
│                 ┌──────▼───────▼──────┐       │                  │
│                 │  stream-switcher    │───────┘                  │
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
  - `/live/offline`: Receives offline video loop
  - `/live/cam`: Receives camera feed (dev mode)
  - `/live/program`: Receives final output from stream-switcher
- **Key Features**: 
  - Stream statistics via XML endpoint
  - Health monitoring
  - 10-second buffer for smooth playback

---

### 2. **ffmpeg-offline** (Always Active)
- **Purpose**: Continuously streams a pre-recorded video file in a loop
- **Technology**: FFmpeg
- **Input**: `offline.mp4` from host filesystem
- **Output**: `rtmp://nginx-rtmp:1936/live/offline`
- **Profile**: All (runs in both production and dev mode)
- **Use Case**: Fallback content when camera is unavailable

---

### 3. **ffmpeg-dev-cam** (Development Only)
- **Purpose**: Simulates a camera feed using a second video file
- **Technology**: FFmpeg
- **Input**: `offline2.mp4` from host filesystem
- **Output**: `rtmp://nginx-rtmp:1936/live/cam`
- **Profile**: `dev` (only runs with `--profile dev`)
- **Use Case**: Testing camera switching logic without real camera hardware

---

### 4. **stream-switcher** (Scene Switcher)
- **Purpose**: Dynamically switches between multiple input streams and outputs a single program stream
- **Technology**: GStreamer with Python control API
- **API Port**: `8088`
- **Inputs**: 
  - `rtmp://nginx-rtmp:1936/live/offline`
  - `rtmp://nginx-rtmp:1936/live/cam`
- **Output**: `rtmp://nginx-rtmp:1936/live/program`
- **Key Features**:
  - RESTful API for manual switching (`/switch?src=offline` or `src=cam`)
  - Seamless transitions using input-selector element
  - Re-encodes to H.264/AAC at 1080p30, 3000 kbps

---

### 5. **stream-auto-switcher** (Automated Quality Manager)
- **Purpose**: Monitors stream quality and automatically switches between camera and offline content
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

### 6. **ffmpeg-kick** (External Relay)
- **Purpose**: Relays the final program stream to Kick streaming platform
- **Technology**: FFmpeg with RTMPS
- **Input**: `rtmp://nginx-rtmp:1936/live/program`
- **Output**: Kick RTMPS server (configured via environment variables)
- **Key Features**:
  - 5-second RTMP buffer for smooth playback
  - Handles Kick-specific streaming requirements
  - Verbose logging for diagnostics

---

### 7. **stream-controller** (Container Manager)
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

### 8. **stream-dashboard** (Web Interface)
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
1. `offline.mp4` → **ffmpeg-offline** → nginx-rtmp `/live/offline`
2. `offline2.mp4` → **ffmpeg-dev-cam** → nginx-rtmp `/live/cam`
3. nginx-rtmp streams → **stream-switcher** → nginx-rtmp `/live/program`
4. nginx-rtmp `/live/program` → **ffmpeg-kick** → Kick streaming platform

### Control Flow:
1. **stream-auto-switcher** monitors nginx-rtmp stats
2. **stream-auto-switcher** sends switch commands to **stream-switcher** API
3. **stream-dashboard** queries **stream-controller** for container status
4. **stream-dashboard** queries nginx-rtmp for stream statistics
5. **stream-dashboard** queries **stream-switcher** for current scene

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
  - `1936`: RTMP streaming
  - `8080`: NGINX stats
  - `8088`: Stream switcher API
  - `8089`: Container controller API
  - `3000`: Web dashboard

---

## Startup Dependencies

```
nginx-rtmp (healthy)
    ├── ffmpeg-offline
    ├── ffmpeg-dev-cam (dev profile)
    └── stream-switcher (after ffmpeg-offline)
            ├── stream-auto-switcher (healthy, auto profile)
            └── ffmpeg-kick (healthy)

stream-controller (independent)
    └── stream-dashboard (after nginx-rtmp and stream-controller healthy)
```

---

## Profiles

- **Default**: nginx-rtmp, ffmpeg-offline, stream-switcher, ffmpeg-kick, stream-controller, stream-dashboard
- **dev**: Adds ffmpeg-dev-cam for camera simulation
- **auto**: Adds stream-auto-switcher for automatic quality management

Profiles can be combined: `docker compose --profile dev --profile auto up -d`

---

## Health Checks

All critical services implement health endpoints:
- **nginx-rtmp**: `http://localhost:8080/health`
- **stream-switcher**: `http://localhost:8088/health`
- **stream-controller**: `http://localhost:8089/health`
- **stream-dashboard**: `http://localhost:3000/api/health`

Health checks ensure proper startup ordering and service reliability.