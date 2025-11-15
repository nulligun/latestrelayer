# Relayer - Stream Management System

A comprehensive stream management system with a compositor, controller, and dashboard for managing video streams and fallback sources.

## Overview

This system provides:
- **Compositor**: GStreamer-based video mixer with SRT input and fallback support
- **Stream Controller**: REST API for managing Docker containers
- **Stream Dashboard**: Web interface for monitoring and controlling streams
- **Offline Sources**: Containers for image, video, and browser-based fallback content

## Quick Start

### Prerequisites

- Docker and Docker Compose installed
- Sufficient disk space for container images

### Initial Setup

1. **Clone the repository and navigate to the project directory**

2. **Configure environment variables**
   ```bash
   cp .env.example .env
   # Edit .env with your configuration
   ```

3. **Build all container images** (REQUIRED for offline sources)
   ```bash
   # Build all services
   docker compose build
   
   # Or build specific services
   docker compose build compositor stream-controller stream-dashboard
   docker compose build offline-image offline-video offline-browser
   ```
   
   **Important**: The offline source containers (offline-image, offline-video, offline-browser) 
   must be built before they can be started via the dashboard. These containers have the 
   `manual` profile and won't be built automatically during `docker compose up`.

4. **Start the core services**
   ```bash
   docker compose up -d compositor stream-controller stream-dashboard
   ```

5. **Access the dashboard**
   - Open your browser to `http://localhost:3000` (or configured port)
   - The dashboard will show all available containers and their status

## Container Management

### Core Services (Auto-start)

- **compositor**: Video mixing and SRT input
- **stream-controller**: Container management API
- **stream-dashboard**: Web UI and monitoring

These start automatically with `docker compose up -d`.

### Manual Services (On-demand)

- **offline-image**: Static image fallback source
- **offline-video**: Looping video fallback source  
- **offline-browser**: Web page rendering fallback source

These containers have the `manual` profile and are started/stopped via the dashboard 
as needed for fallback sources.

### Starting/Stopping Containers

**Via Dashboard (Recommended)**:
- Navigate to the dashboard at `http://localhost:3000`
- Use the "Fallback Source" controls to switch between sources
- Containers will start/stop automatically as needed

**Via CLI**:
```bash
# Start a manual service
docker compose start offline-image

# Stop a manual service  
docker compose stop offline-image

# Restart a service
docker compose restart compositor
```

## Fallback Sources

The system supports multiple fallback sources when the main stream is unavailable:

1. **BLACK**: No container needed, shows black screen
2. **IMAGE**: Uses `offline-image` container to stream a static image
3. **VIDEO**: Uses `offline-video` container to loop a video file
4. **BROWSER**: Uses `offline-browser` container to render a web page

### Uploading Fallback Media

Use the dashboard to upload:
- **Images**: PNG, JPG, GIF (up to 100MB)
- **Videos**: MP4, MOV, MPEG (up to 500MB)

Uploaded files are stored in the `./shared` directory and automatically used by 
the offline containers.

## Troubleshooting

### Offline Containers Won't Start

**Error**: `path "/app/offline-image" not found`

**Solution**: The offline container images must be built before they can be used:
```bash
docker compose build offline-image offline-video offline-browser
```

### Container Status Shows "Not Created"

This is normal for manual profile containers. They will be created and started 
automatically when you select them as a fallback source in the dashboard.

### Checking Container Logs

**Via Dashboard**:
- Click on any container to view its logs in real-time

**Via CLI**:
```bash
# View logs for a specific container
docker compose logs -f offline-image

# View logs for all services
docker compose logs -f
```

## Development

### Project Structure

```
relayer/
├── compositor/          # GStreamer video compositor
├── stream-controller/   # Container management API
├── stream-dashboard/    # Web UI and backend
├── offline-image/       # Static image source
├── offline-video/       # Video loop source
├── offline-browser/     # Browser rendering source
├── shared/              # Shared media and config
└── docker-compose.yml   # Service definitions
```

### Rebuilding After Changes

```bash
# Rebuild and restart a specific service
docker compose up -d --build compositor

# Rebuild all services
docker compose build
docker compose up -d
```

## Configuration

Key environment variables in `.env`:

- `SRT_PORT`: SRT listener port (default: 1937)
- `DASHBOARD_PORT`: Web dashboard port (default: 3000)
- `FALLBACK_SOURCE`: Initial fallback source (BLACK, IMAGE, VIDEO, BROWSER)
- `OFFLINE_SOURCE_URL`: URL for browser-based fallback

See individual service README files for detailed configuration options.

## Architecture

```
┌─────────────┐
│   Browser   │
│  Dashboard  │
└──────┬──────┘
       │ HTTP/WS
       ▼
┌─────────────────┐      ┌──────────────┐
│ Stream          │◀────▶│ Stream       │
│ Dashboard       │      │ Controller   │
└────────┬────────┘      └──────┬───────┘
         │                      │ Docker API
         │ HTTP                 ▼
         ▼              ┌───────────────┐
┌─────────────────┐    │ Compositor    │
│ Compositor API  │◀───│               │
└─────────────────┘    └───────┬───────┘
                               │
                    ┌──────────┼──────────┐
                    ▼          ▼          ▼
              ┌──────────┬──────────┬──────────┐
              │ Offline  │ Offline  │ Offline  │
              │  Image   │  Video   │ Browser  │
              └──────────┴──────────┴──────────┘
```

## License

[Add your license information here]

## Support

For issues or questions, please refer to the individual service README files:
- [Compositor README](compositor/README.md)
- [Stream Controller README](stream-controller/IMPLEMENTATION_PLAN.md)
- [Stream Dashboard README](stream-dashboard/README.md)
- [Offline Image README](offline-image/README.md)
- [Offline Video README](offline-video/README.md)
- [Offline Browser README](offline-browser/README.md)