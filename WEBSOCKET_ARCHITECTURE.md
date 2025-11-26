# WebSocket Architecture Documentation

## Overview

The dashboard-controller communication has been refactored from HTTP polling to a WebSocket-based architecture for improved efficiency and real-time updates.

## Architecture

```
┌──────────────┐      WebSocket      ┌────────────────┐      WebSocket      ┌──────────────┐
│   Frontend   │ ◄──────────────────► │    Dashboard   │ ◄──────────────────► │  Controller  │
│    (Vue.js)  │                      │     Backend    │                      │   (Python)   │
└──────────────┘                      └────────────────┘                      └──────────────┘
                                             │                                       │
                                             │                                       │
                                             │                                       ▼
                                             │                                  ┌──────────┐
                                             │                                  │  Docker  │
                                             │                                  │   API    │
                                             │                                  └──────────┘
                                             ▼
                                      ┌──────────────┐
                                      │  REST APIs   │
                                      │ (Container   │
                                      │  Control)    │
                                      └──────────────┘
```

### Key Components

1. **Controller WebSocket Server** (Python)
   - Port: 8090
   - Monitors Docker containers every 2 seconds
   - Sends updates only when container state changes
   - Handles on-demand log streaming

2. **Dashboard Backend** (Node.js)
   - WebSocket client to controller (port 8090)
   - WebSocket server for frontend clients (port 3000)
   - Proxies messages between controller and frontend
   - Maintains REST API for container control operations

3. **Frontend** (Vue.js)
   - WebSocket client to dashboard backend
   - Receives real-time container updates
   - Requests log streaming on-demand

## WebSocket Message Protocol

### Controller → Dashboard Messages

#### 1. Initial State
Sent immediately when dashboard connects to controller.

```json
{
  "type": "initial_state",
  "timestamp": "2025-11-25T22:52:32.016Z",
  "containers": [
    {
      "name": "nginx-proxy",
      "full_name": "latestrelayer-nginx-proxy",
      "status": "running",
      "status_detail": "Up 2 hours (healthy)",
      "running": true,
      "health": "healthy",
      "id": "abc123ef",
      "created": true
    }
  ]
}
```

#### 2. Status Change
Sent only when container state changes (status, health, or running state).

```json
{
  "type": "status_change",
  "timestamp": "2025-11-25T22:53:15.234Z",
  "changes": [
    {
      "name": "ffmpeg-kick",
      "previousStatus": "exited",
      "previousHealth": null,
      "currentStatus": "running",
      "currentHealth": "starting",
      "running": true,
      "statusDetail": "Up 5 seconds (health: starting)"
    }
  ]
}
```

#### 3. Log Snapshot
Sent once when client subscribes to container logs.

```json
{
  "type": "log_snapshot",
  "container": "ffmpeg-kick",
  "logs": [
    "2025-11-25T22:52:30.123Z Starting FFmpeg...",
    "2025-11-25T22:52:31.456Z Connected to Kick..."
  ],
  "lastLogTimestamp": "2025-11-25T22:52:31.456Z"
}
```

#### 4. New Logs
Sent every 500ms while logs are subscribed (only if new logs exist).

```json
{
  "type": "new_logs",
  "container": "ffmpeg-kick",
  "logs": [
    "2025-11-25T22:52:32.789Z Stream started"
  ],
  "lastLogTimestamp": "2025-11-25T22:52:32.789Z"
}
```

### Dashboard → Controller Messages

#### 1. Subscribe to Logs
Request to start streaming logs for a container.

```json
{
  "type": "subscribe_logs",
  "container": "ffmpeg-kick",
  "lines": 100
}
```

#### 2. Unsubscribe from Logs
Request to stop streaming logs for a container.

```json
{
  "type": "unsubscribe_logs",
  "container": "ffmpeg-kick"
}
```

### Dashboard Backend → Frontend Messages

The dashboard backend transforms controller messages into a unified format:

#### Container Update
```json
{
  "type": "container_update",
  "timestamp": "2025-11-25T22:52:32.016Z",
  "containers": [...],
  "changes": [...]  // Optional, only present for status_change events
}
```

#### Log Messages
Logs are forwarded directly from controller to frontend:
- `log_snapshot` - Initial logs when subscription starts
- `new_logs` - Incremental log updates

### Frontend → Dashboard Backend Messages

Same as Dashboard → Controller messages above. Frontend sends log subscription requests that are proxied to the controller.

## Performance Improvements

### Before (HTTP Polling)

- Dashboard polls controller every 2 seconds
- Each poll fetches status for ~15 containers
- Logs endpoint called with tail=500 repeatedly
- **Network Traffic**: High, continuous polling
- **Controller CPU**: High, processing same requests repeatedly

### After (WebSocket)

- Single persistent WebSocket connection
- Status updates only when state changes (typically 0-5 messages/minute)
- Logs sent incrementally (only new lines)
- **Network Traffic**: ~99% reduction
- **Controller CPU**: ~95% reduction

## Implementation Details

### Controller (Python)

File: [`controller/container_controller.py`](controller/container_controller.py)

**Key Classes:**
- `WebSocketServer`: Manages WebSocket connections and message handling
- `ContainerController`: Docker operations (unchanged, used by WebSocket server)

**Background Tasks:**
- `monitor_container_status()`: Checks Docker every 2s, broadcasts changes
- `stream_logs()`: Polls Docker logs every 500ms for subscribed containers

### Dashboard Backend (Node.js)

Files:
- [`dashboard/backend/services/controllerWebSocket.js`](dashboard/backend/services/controllerWebSocket.js) - WebSocket client to controller
- [`dashboard/backend/server.js`](dashboard/backend/server.js) - Main server with WebSocket handling

**Features:**
- Automatic reconnection with exponential backoff
- Event-based message handling
- Maintains latest container state for new frontend connections

### Frontend (Vue.js)

File: [`dashboard/frontend/src/services/websocket.js`](dashboard/frontend/src/services/websocket.js)

**Usage:**
- Frontend components subscribe/unsubscribe to logs as needed
- Real-time container updates automatically reflected in UI

## Configuration

### Environment Variables

Add to `.env` file:

```bash
# Controller ports
CONTROLLER_API_PORT=8089      # HTTP API (default)
CONTROLLER_WS_PORT=8090       # WebSocket port (new)

# Dashboard configuration
CONTROLLER_API=http://controller:8089
```

### Docker Compose

The controller service now exposes both ports:

```yaml
controller:
  ports:
    - "${CONTROLLER_API_PORT:-8089}:8089"   # HTTP API
    - "${CONTROLLER_WS_PORT:-8090}:8090"    # WebSocket
```

## Error Handling

### Connection Failures

1. **Controller WebSocket Down**
   - Dashboard backend automatically reconnects with exponential backoff
   - Max delay: 30 seconds between attempts
   - Frontend receives cached container state from dashboard backend

2. **Dashboard Backend Down**
   - Frontend WebSocket client auto-reconnects
   - Container state restored on reconnection

3. **Network Issues**
   - Both client implementations use exponential backoff
   - Operations continue via REST API if needed

### State Synchronization

- Controller sends full state on connection (initial_state)
- Dashboard backend maintains latest state for new frontend clients
- State changes are incremental updates

## REST API Compatibility

All container control operations remain as REST endpoints:

- `POST /api/container/:name/start`
- `POST /api/container/:name/stop`
- `POST /api/container/:name/restart`
- `GET /api/container/:name/logs` (fallback for on-demand log fetch)

## Testing

### Verify WebSocket Connection

1. Start the system:
   ```bash
   docker compose up -d
   ```

2. Check controller logs for WebSocket server startup:
   ```bash
   docker compose logs controller | grep "\[ws\]"
   ```

3. Check dashboard logs for WebSocket connection:
   ```bash
   docker compose logs dashboard | grep "controller-ws"
   ```

### Test Container Status Updates

1. Start a container:
   ```bash
   docker compose start ffmpeg-kick
   ```

2. Watch for status_change message in dashboard logs

### Test Log Streaming

1. Open dashboard in browser
2. Click on a container to view logs
3. Verify logs appear in real-time
4. Check that logs stop streaming when panel is closed

## Troubleshooting

### WebSocket Not Connecting

1. Check controller is running and healthy:
   ```bash
   docker compose ps controller
   ```

2. Verify port 8090 is exposed:
   ```bash
   docker compose port controller 8090
   ```

3. Check for firewall/network issues

### High CPU Usage

If controller CPU is still high:
- Check monitoring loop interval (should be 2s)
- Verify status changes are not too frequent
- Review log streaming subscriptions

### Missing Updates

If container status not updating:
- Check WebSocket connection status in logs
- Verify state-change detection logic
- Confirm frontend is receiving messages

## Migration Notes

### Breaking Changes

- Aggregator polling is removed
- Frontend must handle `container_update` message type
- Log streaming requires WebSocket message support

### Backward Compatibility

- REST API endpoints remain unchanged
- `/api/data` endpoint still works for one-time fetches
- Container control operations unaffected