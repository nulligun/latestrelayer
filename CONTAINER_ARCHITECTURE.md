# Container Architecture - Simplified System

## Overview
The SRT Stream Relay System has been simplified from 10 containers to just 3 containers, consolidating all streaming functionality into a single Python/GStreamer process.

## Container Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        HOST SYSTEM                               │
│  ┌──────────────┐              ┌──────────────┐                 │
│  │ offline.mp4  │              │ External SRT │                 │
│  │ (fallback)   │              │ Source       │                 │
│  └──────┬───────┘              └──────┬───────┘                 │
└─────────┼──────────────────────────────┼──────────────────────────┘
          │                              │ SRT Port 1937 (UDP)
          │                              │
┌─────────▼──────────────────────────────▼──────────────────────────┐
│                     DOCKER NETWORK (rtmp-network)                 │
│                                                                    │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │              srt-switcher (Single Process)                 │  │
│  │           Python 3 + GStreamer Pipeline                    │  │
│  │                                                            │  │
│  │  ┌─────────────┐        ┌─────────────┐                  │  │
│  │  │ SRT Branch  │───────►│             │                  │  │
│  │  │ srtsrc      │        │   Input     │                  │  │
│  │  │ decodebin   │        │  Selector   │                  │  │
│  │  │ normalize   │        │   (video    │                  │  │
│  │  └─────────────┘        │  & audio)   │                  │  │
│  │                         │             │                  │  │
│  │  ┌─────────────┐        │             │                  │  │
│  │  │ Fallback    │───────►│             │                  │  │
│  │  │ filesrc     │        └──────┬──────┘                  │  │
│  │  │ decodebin   │               │                         │  │
│  │  │ normalize   │               │                         │  │
│  │  │ (looping)   │               ▼                         │  │
│  │  └─────────────┘        ┌─────────────┐                  │  │
│  │                         │  Encoder    │                  │  │
│  │  HTTP API: 8088        │  x264+AAC   │                  │  │
│  │  /health               │  FLV mux    │                  │  │
│  │  /scene                 └──────┬──────┘                  │  │
│  │  /switch?src=srt|fallback      │                         │  │
│  └────────────────────────────────┼─────────────────────────┘  │
│                                   │ RTMPS                       │
│                                   ▼                             │
│                          ┌──────────────────┐                   │
│                          │   Kick Server    │                   │
│                          │   (External)     │                   │
│                          └──────────────────┘                   │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │            MONITORING & CONTROL LAYER                     │  │
│  │                                                            │  │
│  │  ┌──────────────────────┐     ┌──────────────────────┐   │  │
│  │  │ stream-controller    │     │ stream-dashboard     │   │  │
│  │  │                      │     │                      │   │  │
│  │  │ Port: 8089           │     │ Port: 3000           │   │  │
│  │  │ Docker socket access │◄────┤ Vue.js UI            │   │  │
│  │  │ Container lifecycle  │     │ WebSocket updates    │   │  │
│  │  │ management           │     │ Real-time metrics    │   │  │
│  │  └──────────────────────┘     └──────────────────────┘   │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

## Container Details

### 1. **srt-switcher** (Streaming Core)
- **Purpose**: All-in-one streaming solution with automatic failover
- **Technology**: Python 3 + GStreamer 1.24+
- **Ports**:
  - `1937`: SRT listener (UDP)
  - `8088`: HTTP API
- **Input Sources**:
  - SRT listener on port 9000 (internal)
  - Looping video file (`/videos/fallback.mp4`)
- **Output**: Direct RTMPS to Kick
- **Key Features**:
  - Automatic source detection and switching
  - Instant transitions via input-selector
  - No pipeline restarts required
  - Full audio/video synchronization
  - RESTful API for control
- **API Endpoints**:
  - `GET /health` - Pipeline status and uptime
  - `GET /scene` - Current active source
  - `GET /switch?src=srt|fallback` - Manual source selection
- **State Management**:
  - Monitors SRT connection via pad-added signals
  - Detects disconnection via ERROR bus messages
  - Handles EOS (end-of-stream) for looping
  - Maintains continuous output regardless of input state

---

### 2. **stream-controller** (Container Manager)
- **Purpose**: REST API for container lifecycle management
- **Technology**: Python with Docker SDK
- **Port**: `8089`
- **Access**: Docker socket (`/var/run/docker.sock`)
- **Operations**:
  - Start/Stop/Restart containers
  - Query container status
  - List all managed containers
- **Endpoints**:
  - `POST /container/{name}/start`
  - `POST /container/{name}/stop`
  - `POST /container/{name}/restart`
  - `GET /container/{name}/status`
  - `GET /containers`
  - `GET /health`

---

### 3. **stream-dashboard** (Web Interface)
- **Purpose**: Web-based monitoring and control interface
- **Technology**: Vue.js 3 + Express.js + WebSocket
- **Port**: `3000` (HTTP + WebSocket)
- **Features**:
  - **Real-time Monitoring**:
    - System metrics (CPU, memory, load)
    - Stream status from srt-switcher
    - Current active scene
  - **Container Control**:
    - Visual start/stop/restart buttons
    - Container status indicators
  - **Live Updates**: WebSocket connection with 2-second polling
- **Architecture**:
  - Frontend: Vue.js single-page application
  - Backend: Express.js aggregating data from services
  - Services: Controller API, srt-switcher API

---

## Data Flow

### Video Stream Flow:
1. **External SRT Source** → **srt-switcher** (SRT branch) → selector → encoder → Kick
2. **Fallback Video** → **srt-switcher** (file branch) → selector → encoder → Kick

The input-selector in srt-switcher determines which branch feeds the encoder at any given time.

### Control Flow:
1. **stream-dashboard** queries **srt-switcher** `/health` for stream status
2. **stream-dashboard** queries **stream-controller** for container status
3. **stream-dashboard** sends control commands to **stream-controller**
4. **stream-controller** manages containers via Docker socket

### Management Flow:
1. User accesses **stream-dashboard** web UI (port 3000)
2. Dashboard displays real-time status from both APIs
3. User clicks container control buttons
4. Commands sent to **stream-controller** (port 8089)
5. **stream-controller** executes Docker operations
6. Dashboard receives updates via WebSocket

---

## Network Configuration

- **Network Name**: `rtmp-network`
- **Type**: Docker bridge network
- **Internal Communication**: All containers communicate using service names
- **External Access**: Only specific ports exposed to host:
  - `1937`: SRT input (srt-switcher listener)
  - `8088`: Stream API (srt-switcher)
  - `8089`: Container controller API
  - `3000`: Web dashboard

---

## Startup Dependencies

```
srt-switcher (independent, healthy check on port 8088)
    └── stream-dashboard (depends on srt-switcher + stream-controller healthy)

stream-controller (independent, healthy check on port 8089)
    └── stream-dashboard (depends on srt-switcher + stream-controller healthy)
```

All services can start independently, but the dashboard waits for both APIs to be healthy.

---

## Health Checks

All services implement health endpoints:
- **srt-switcher**: `http://localhost:8088/health`
  - Returns: Pipeline state, current source, SRT connection status, uptime
- **stream-controller**: `http://localhost:8089/health`
  - Returns: Service status
- **stream-dashboard**: `http://localhost:3000/api/health`
  - Returns: Dashboard backend status

---

## Advantages Over Previous Architecture

### Before: 10 Containers
- nginx-rtmp (RTMP relay hub)
- ffmpeg-brb (BRB video streamer)
- ffmpeg-cam-dev (dev camera simulator)
- ffmpeg-srt (SRT to RTMP bridge)
- ffmpeg-cam-normalized (stream normalizer)
- muxer (GStreamer switcher)
- ffmpeg-kick (Kick pusher)
- stream-auto-switcher (quality monitor)
- stream-controller
- stream-dashboard

### After: 3 Containers
- **srt-switcher** (combines 7 streaming containers)
- stream-controller (unchanged)
- stream-dashboard (unchanged)

### Benefits

1. **Simplicity**: 70% fewer containers
2. **Performance**: 
   - Direct path: SRT → encode → Kick
   - No RTMP intermediary
   - Lower latency (eliminates 2-3 relay hops)
3. **Resource Efficiency**:
   - One GStreamer process vs multiple FFmpeg processes
   - Reduced memory footprint
   - Lower CPU overhead
4. **Reliability**:
   - Fewer points of failure
   - No network relay dependencies
   - Single state machine for switching
5. **Maintainability**:
   - All streaming logic in one codebase
   - Easier debugging
   - Simpler configuration

### Technical Improvements

1. **Instant Switching**: GStreamer input-selector provides frame-accurate transitions without re-encoding
2. **Auto-Recovery**: Built-in SRT connection monitoring with automatic fallback
3. **Continuous Output**: Pipeline never stops, only source selection changes
4. **Type Safety**: Python with strong typing for control logic
5. **Clear State**: Single source of truth for current source and connection status

---

## Migration Notes

### Removed Components
- nginx-rtmp (no longer needed for relay)
- All ffmpeg-* containers (consolidated into srt-switcher)
- muxer (replaced by srt-switcher)
- stream-auto-switcher (switching logic now internal)

### Configuration Changes
- `MUXER_API_PORT` now points to srt-switcher (backward compatible)
- `SRT_PORT` now directly maps to srt-switcher
- Removed RTMP-related environment variables
- Simplified video encoding parameters

### API Compatibility
The srt-switcher provides API compatibility with the old muxer:
- `/health` endpoint (enhanced with more details)
- `/scene` endpoint (same format)
- `/switch?src=` endpoint (now uses `srt` and `fallback` instead of `cam` and `brb`)

---

## Future Enhancements

Potential improvements to the current architecture:

1. **Multiple Outputs**: Add simultaneous streaming to multiple platforms
2. **Recording**: Add local file recording capability
3. **Transcoding Profiles**: Multiple quality levels for different platforms
4. **Advanced Monitoring**: Metrics export (Prometheus/Grafana)
5. **Overlay Support**: Add text/image overlays to stream
6. **Multi-Source**: Support more than 2 input sources