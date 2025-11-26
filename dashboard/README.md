# Dashboard

A real-time web dashboard for monitoring and controlling the RTMP streaming infrastructure.

## Features

- **Real-time Monitoring**: Live updates via WebSocket every 2 seconds
- **System Metrics**: CPU usage, memory usage, and system load
- **RTMP Statistics**: Current inbound bandwidth from camera streams
- **Stream Status**: Active scene indicator (offline/cam)
- **Container Control**: Start, stop, and restart any container
- **Responsive UI**: Modern Vue.js interface with dark theme

## Architecture

### Backend (Express + WebSocket)
- REST API for container operations
- WebSocket server for real-time data push
- Aggregates data from:
  - controller API (container status)
  - nginx-rtmp stats (bandwidth, streams)
  - stream-switcher API (current scene)
  - System metrics (CPU, memory, load)

### Frontend (Vue.js 3)
- Single-page application
- Real-time updates via WebSocket
- Responsive grid layout
- Component-based architecture

## Configuration

Environment variables (set in `.env` or docker-compose):

```bash
# Port
DASHBOARD_PORT=3000

# API Endpoints (internal Docker network)
CONTROLLER_API=http://controller:8089
MUXER_API=http://stream-switcher:8088
NGINX_STATS=http://nginx-rtmp:8080/stat

# Polling interval (milliseconds)
DASHBOARD_POLLING_INTERVAL=2000
```

## Usage

### Start the dashboard
```bash
docker compose up -d dashboard
```

### Access the dashboard
Open your browser to:
```
http://localhost:3000
```

### View logs
```bash
docker compose logs -f dashboard
```

## Development

### Local Development

**Backend:**
```bash
cd dashboard/backend
npm install
npm run dev
```

**Frontend:**
```bash
cd dashboard/frontend
npm install
npm run dev
```

The frontend will be available at `http://localhost:5173` during development.

### Build for Production

```bash
docker compose build dashboard
```

## API Endpoints

### REST API

- `GET /api/health` - Health check
- `GET /api/data` - Get current aggregated data (one-time)
- `POST /api/container/:name/start` - Start a container
- `POST /api/container/:name/stop` - Stop a container
- `POST /api/container/:name/restart` - Restart a container

### WebSocket

Connect to `ws://localhost:3000` to receive real-time updates.

**Message Format:**
```json
{
  "timestamp": "2025-10-31T16:30:00.000Z",
  "containers": [
    {
      "name": "ffmpeg-kick",
      "fullName": "relayer-ffmpeg-kick",
      "status": "running",
      "running": true,
      "id": "abc123"
    }
  ],
  "systemMetrics": {
    "cpu": 45.2,
    "memory": 68.5,
    "load": [1.2],
    "memoryUsed": 8.5,
    "memoryTotal": 16.0
  },
  "rtmpStats": {
    "inboundBandwidth": 3500,
    "streams": {
      "offline": {
        "active": true,
        "bandwidth": 2500,
        "clients": 2,
        "publishing": true
      },
      "cam": {
        "active": true,
        "bandwidth": 3500,
        "clients": 1,
        "publishing": true
      }
    }
  },
  "currentScene": "cam"
}
```

## Troubleshooting

### Dashboard not connecting
- Check if the service is running: `docker compose ps dashboard`
- Check logs: `docker compose logs dashboard`
- Verify port 3000 is not in use by another service

### No data showing
- Ensure controller is running and healthy
- Ensure nginx-rtmp is running and accessible
- Check the backend logs for API connection errors

### WebSocket disconnecting
- Check browser console for errors
- Verify network stability
- Check if backend is under heavy load