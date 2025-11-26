# Container Controller Service - Implementation Plan

## Overview
A lightweight HTTP API service that provides REST endpoints to control Docker containers in the relayer system. Built using Python's built-in http.server (no external dependencies) and the Docker SDK.

## Architecture

```mermaid
graph TB
    subgraph External
        CLIENT[REST API Client]
    end
    
    subgraph controller Container
        HTTP[HTTP Server<br/>Port 8089]
        DOCKER[Docker SDK Client]
        HTTP --> DOCKER
    end
    
    subgraph Docker Socket
        SOCKET[/var/run/docker.sock]
    end
    
    subgraph Managed Containers
        KICK[ffmpeg-kick]
        OFFLINE[ffmpeg-offline]
        CAM[ffmpeg-cam-dev]
        SWITCHER[muxer]
    end
    
    CLIENT -->|POST /container/ffmpeg-kick/start| HTTP
    CLIENT -->|POST /container/ffmpeg-kick/stop| HTTP
    CLIENT -->|GET /container/ffmpeg-kick/status| HTTP
    CLIENT -->|GET /health| HTTP
    
    DOCKER --> SOCKET
    SOCKET --> KICK
    SOCKET --> OFFLINE
    SOCKET --> CAM
    SOCKET --> SWITCHER
```

## API Specification

### Endpoints

| Method | Path | Description | Response |
|--------|------|-------------|----------|
| POST | `/container/<name>/start` | Start a container | `{"status": "started", "container": "name"}` |
| POST | `/container/<name>/stop` | Stop a container | `{"status": "stopped", "container": "name"}` |
| POST | `/container/<name>/restart` | Restart a container | `{"status": "restarted", "container": "name"}` |
| GET | `/container/<name>/status` | Get container status | `{"container": "name", "status": "running", "running": true}` |
| GET | `/containers` | List all managed containers | `{"containers": [...]}` |
| GET | `/health` | Service health check | `"ok"` |

### Container Names
The service will manage containers with the project prefix (`relayer-` by default):
- `ffmpeg-kick`
- `ffmpeg-offline`
- `ffmpeg-cam-dev`
- `muxer`
- `nginx-rtmp`

### Example Usage

```bash
# Start the kick stream
curl -X POST http://localhost:8089/container/ffmpeg-kick/start

# Stop the kick stream
curl -X POST http://localhost:8089/container/ffmpeg-kick/stop

# Check kick stream status
curl http://localhost:8089/container/ffmpeg-kick/status

# Restart a container
curl -X POST http://localhost:8089/container/ffmpeg-kick/restart

# List all containers
curl http://localhost:8089/containers

# Health check
curl http://localhost:8089/health
```

## Implementation Details

### File Structure
```
controller/
├── Dockerfile              # Container image definition
├── container_controller.py # Main Python script
└── IMPLEMENTATION_PLAN.md  # This file
```

### Key Components

#### 1. Dockerfile
- Base image: `python:3.11-slim`
- Install `docker` Python package via pip
- Copy controller script
- Expose port 8089
- Run with unbuffered output

#### 2. container_controller.py
**Pattern:** Similar to [`rtmp_switcher.py`](../muxer/rtmp_switcher.py)

**Structure:**
```python
#!/usr/bin/env python3
import sys
import json
import docker
from urllib.parse import urlparse
from http.server import BaseHTTPRequestHandler, HTTPServer

# Force unbuffered output
sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

class ContainerController:
    def __init__(self, project_name="relayer"):
        self.client = docker.from_env()
        self.project_name = project_name
    
    def get_container_name(self, short_name):
        return f"{self.project_name}-{short_name}"
    
    def start_container(self, short_name):
        # Implementation
    
    def stop_container(self, short_name):
        # Implementation
    
    def restart_container(self, short_name):
        # Implementation
    
    def get_status(self, short_name):
        # Implementation
    
    def list_containers(self):
        # Implementation

class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Suppress default logging
    
    def do_POST(self):
        # Handle POST requests for start/stop/restart
    
    def do_GET(self):
        # Handle GET requests for status/health/list
    
    def send_json(self, data, status=200):
        # Helper to send JSON responses
```

**Features:**
- Docker SDK integration for container management
- JSON response format for easy API consumption
- Error handling with appropriate HTTP status codes
- Logging with structured output (similar to rtmp_switcher)
- Project name prefix support for multi-instance deployments

#### 3. Docker Compose Integration

Add to [`docker-compose.yml`](../docker-compose.yml):

```yaml
  controller:
    build:
      context: ./controller
      dockerfile: Dockerfile
    container_name: ${COMPOSE_PROJECT_NAME:-relayer}-controller
    ports:
      - "${CONTROLLER_API_PORT:-8089}:8089"
    volumes:
      - /var/run/docker.sock:/var/run/docker.sock:ro
    networks:
      - rtmp-network
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:8089/health"]
      interval: 10s
      timeout: 5s
      retries: 3
      start_period: 5s
```

**Key Configuration:**
- Read-only Docker socket mount (`:ro` for security)
- Port configurable via environment variable
- Independent of other services (no depends_on)
- Health check on `/health` endpoint

#### 4. Environment Variables

Add to [`.env`](../.env):
```bash
# Container Controller API
CONTROLLER_API_PORT=8089
```

## Security Considerations

1. **Docker Socket Access**: Mounted as read-only (`:ro`), but note that Docker SDK still allows full container control
2. **No Authentication**: Currently no auth layer - consider adding if exposed to network
3. **Container Scope**: Only manages containers with the project prefix
4. **Network Isolation**: Runs in same Docker network as managed containers

## Error Handling

The service will handle common errors:

| Scenario | HTTP Status | Response |
|----------|-------------|----------|
| Container not found | 404 | `{"error": "Container not found: name"}` |
| Container already running | 409 | `{"error": "Container already running"}` |
| Container already stopped | 409 | `{"error": "Container already stopped"}` |
| Docker API error | 500 | `{"error": "Docker error: details"}` |
| Invalid endpoint | 404 | `{"error": "Not found"}` |
| Invalid method | 405 | `{"error": "Method not allowed"}` |

## Testing Strategy

### Manual Testing
```bash
# 1. Test health endpoint
curl http://localhost:8089/health

# 2. Test list containers
curl http://localhost:8089/containers

# 3. Test status (container running)
curl http://localhost:8089/container/ffmpeg-kick/status

# 4. Test stop
curl -X POST http://localhost:8089/container/ffmpeg-kick/stop

# 5. Test status (container stopped)
curl http://localhost:8089/container/ffmpeg-kick/status

# 6. Test start
curl -X POST http://localhost:8089/container/ffmpeg-kick/start

# 7. Test restart
curl -X POST http://localhost:8089/container/ffmpeg-kick/restart

# 8. Test invalid container
curl http://localhost:8089/container/invalid-name/status

# 9. Test invalid endpoint
curl http://localhost:8089/invalid
```

### Integration Testing
- Verify kick stream stops when commanded
- Verify kick stream starts and reconnects to RTMP
- Verify other services remain unaffected
- Test multiple rapid start/stop cycles

## Documentation Updates

Update [`README.md`](../README.md) sections:

### 1. Add to Architecture Diagram
Show controller as a separate service

### 2. Add to Services List
```markdown
### 5. controller
- **Purpose**: Container lifecycle management via REST API
- **API Port**: 8089
- **Capabilities**: Start/stop/restart/status for all containers
```

### 3. Add API Usage Section
```markdown
## Container Control API

The stream controller exposes an HTTP API on port 8089:

**Start ffmpeg-kick:**
```bash
curl -X POST "http://localhost:8089/container/ffmpeg-kick/start"
```

**Stop ffmpeg-kick:**
```bash
curl -X POST "http://localhost:8089/container/ffmpeg-kick/stop"
```

**Check status:**
```bash
curl "http://localhost:8089/container/ffmpeg-kick/status"
```

**Health check:**
```bash
curl "http://localhost:8089/health"
```
```

### 4. Update Environment Variables Section
Add `CONTROLLER_API_PORT=8089`

### 5. Add to Troubleshooting
Common issues and solutions for container control

## Implementation Order

1. ✅ Create directory structure
2. ✅ Write Dockerfile
3. ✅ Implement container_controller.py
4. ✅ Update docker-compose.yml
5. ✅ Update .env file
6. ✅ Update README.md
7. ✅ Build and test service
8. ✅ Test all API endpoints
9. ✅ Verify integration with existing services

## Future Enhancements

- Add container logs endpoint: `GET /container/<name>/logs`
- Add container exec endpoint for troubleshooting
- Add metrics/stats endpoint
- Add authentication layer
- Add WebSocket support for real-time status updates
- Add batch operations (start/stop multiple containers)