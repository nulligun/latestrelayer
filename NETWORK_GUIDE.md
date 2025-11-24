# Docker Network Configuration Guide

This guide explains the Docker networking setup for the TSDuck Multiplexer project and how containers communicate with each other.

## Network Overview

All containers run on a single Docker bridge network: `tsduck-multiplexer-network`

```
┌─────────────────────────────────────────────────────────────┐
│            tsduck-multiplexer-network (bridge)              │
│                                                             │
│  ┌──────────────┐    ┌──────────────┐   ┌───────────────┐ │
│  │ ts-multiplexer│◄───│ffmpeg-srt-live│   │ffmpeg-fallback│ │
│  │              │    │              │   │               │ │
│  │ Service:     │    │Service:      │   │Service:       │ │
│  │ multiplexer  │    │ffmpeg-srt-live   │ffmpeg-fallback│ │
│  └──────┬───────┘    └──────────────┘   └───────────────┘ │
│         │                                                   │
│         │ RTMP                                              │
│         ▼                                                   │
│  ┌──────────────┐                                          │
│  │nginx-rtmp-   │                                          │
│  │server        │                                          │
│  │              │                                          │
│  │Service:      │                                          │
│  │nginx-rtmp    │                                          │
│  └──────────────┘                                          │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Container Service Names

**CRITICAL**: In Docker Compose, containers communicate using **service names**, NOT container names.

| Service Name (USE THIS) | Container Name (Don't Use) | Purpose |
|-------------------------|----------------------------|---------|
| `multiplexer` | `ts-multiplexer` | TSDuck multiplexer |
| `nginx-rtmp` | `nginx-rtmp-server` | RTMP/HLS server |
| `ffmpeg-srt-live` | `ffmpeg-srt-live` | Live SRT input |
| `ffmpeg-fallback` | `ffmpeg-fallback` | Fallback stream |

### Examples

#### ✅ CORRECT - Using Service Names
```bash
# Ping from multiplexer to nginx
docker exec ts-multiplexer ping nginx-rtmp

# Connect to RTMP server
rtmp://nginx-rtmp/live/test

# Send UDP to multiplexer
udp://multiplexer:10000
```

#### ❌ INCORRECT - Using Container Names
```bash
# This will fail!
docker exec ts-multiplexer ping nginx-rtmp-server

# This will fail!
rtmp://nginx-rtmp-server/live/test
```

## Port Configuration

### Multiplexer UDP Ports
- **10000/udp** - Live stream input (from ffmpeg-srt-live)
- **10001/udp** - Fallback stream input (from ffmpeg-fallback)

### Nginx RTMP Ports
- **1935/tcp** - RTMP input/output (mapped to host)
- **8080/tcp** - HTTP/HLS output (mapped to host)

### External Ports
- **1937/udp** - SRT listener (on ffmpeg-srt-live, mapped to host)

## Network Testing

### Quick Connectivity Test
```bash
# Test 1: Verify containers are on the same network
docker network inspect tsduck-multiplexer-network

# Test 2: Ping nginx from multiplexer (CORRECT)
docker exec ts-multiplexer ping -c 3 nginx-rtmp

# Test 3: Check RTMP port
docker exec ts-multiplexer nc -zv nginx-rtmp 1935

# Test 4: Check UDP ports
docker exec ts-multiplexer ss -uln | grep -E ":(10000|10001)"
```

### Comprehensive Test
Run the automated test script:
```bash
./test-network-connectivity.sh
```

This script will verify:
- All containers are running
- Network configuration is correct
- DNS resolution works with service names
- ICMP connectivity between containers
- RTMP and HTTP ports are accessible
- Configuration uses correct service names

## Configuration Files

### config.yaml
Uses the correct service name for RTMP:
```yaml
rtmp_url: "rtmp://nginx-rtmp/live/test"
```

### docker-compose.yml
Network configuration:
```yaml
networks:
  tsnet:
    driver: bridge
    name: tsduck-multiplexer-network
```

All services must include:
```yaml
services:
  service_name:
    networks:
      - tsnet
```

## Common Issues and Solutions

### Issue: "ping: nginx-rtmp-server: Name or service not known"

**Cause**: Using container name instead of service name

**Solution**: Use service name `nginx-rtmp` instead of container name `nginx-rtmp-server`

```bash
# Wrong
docker exec ts-multiplexer ping nginx-rtmp-server

# Correct
docker exec ts-multiplexer ping nginx-rtmp
```

### Issue: "nginx: [emerg] 'access_log' directive is not allowed here"

**Cause**: `access_log` directive in wrong section of nginx.conf

**Solution**: Remove `access_log` from top level, only use in `http` or `rtmp` blocks

### Issue: Containers can't communicate

**Troubleshooting steps**:
1. Verify all containers are on the same network:
   ```bash
   docker network inspect tsduck-multiplexer-network
   ```

2. Check if containers are running:
   ```bash
   docker compose ps
   ```

3. Verify DNS resolution:
   ```bash
   docker exec ts-multiplexer getent hosts nginx-rtmp
   ```

4. Test connectivity:
   ```bash
   docker exec ts-multiplexer ping -c 3 nginx-rtmp
   ```

5. Check port accessibility:
   ```bash
   docker exec ts-multiplexer nc -zv nginx-rtmp 1935
   ```

## External Access

### RTMP Streaming
Players can connect from outside Docker using:
- **RTMP**: `rtmp://localhost:1935/live/test`
- **HLS**: `http://localhost:8080/hls/test.m3u8`

### SRT Input
Send live SRT stream to:
- `srt://localhost:1937`

## Network IP Addresses

IP addresses are dynamically assigned by Docker. Example configuration:

| Container | IP Address |
|-----------|------------|
| ts-multiplexer | 172.18.0.3/16 |
| nginx-rtmp-server | 172.18.0.5/16 |
| ffmpeg-srt-live | 172.18.0.2/16 |
| ffmpeg-fallback | 172.18.0.4/16 |

**Note**: Always use service names, not IP addresses. IP addresses may change when containers restart.

## Best Practices

1. **Always use service names** for inter-container communication
2. **Use container names** only for docker exec commands
3. **Test connectivity** after any network changes
4. **Check logs** if connections fail: `docker logs <container_name>`
5. **Verify health** of containers: `docker compose ps`

## Quick Reference

### Restart Network
```bash
docker compose down
docker compose up -d
```

### View Logs
```bash
docker logs ts-multiplexer
docker logs nginx-rtmp-server
docker logs ffmpeg-srt-live
docker logs ffmpeg-fallback
```

### Network Inspection
```bash
# View all networks
docker network ls

# Inspect specific network
docker network inspect tsduck-multiplexer-network

# View network for specific container
docker inspect ts-multiplexer --format '{{.NetworkSettings.Networks}}'
```

## Testing Checklist

- [ ] All containers are running
- [ ] All containers are on `tsduck-multiplexer-network`
- [ ] Can ping `nginx-rtmp` from `ts-multiplexer`
- [ ] Can resolve DNS for all service names
- [ ] RTMP port 1935 is accessible
- [ ] HTTP port 8080 is accessible
- [ ] UDP ports 10000 and 10001 are bound on multiplexer
- [ ] Configuration files use service names (not container names)

Run `./test-network-connectivity.sh` to automatically verify all items on this checklist.