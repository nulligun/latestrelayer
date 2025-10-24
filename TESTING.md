# Testing Guide for Refactored RTMP Relay

## Pre-Test Checklist

1. **Remove old ffmpeg-relay directory** (no longer used):
   ```bash
   # Backup if needed
   mv ffmpeg-relay ffmpeg-relay.backup
   ```

2. **Configure your .env file**:
   ```bash
   cp .env.example .env
   # Edit .env with your actual Kick credentials and offline video path
   ```

3. **Ensure offline video exists**:
   ```bash
   # Check the file path in your .env OFFLINE_VIDEO_PATH
   ls -lh /path/to/your/offline.mp4
   ```

## Build and Start

```bash
# Stop any existing containers
docker-compose down

# Rebuild all services
docker-compose build

# Start in detached mode
docker-compose up -d

# Watch logs
docker-compose logs -f
```

## Test Scenario 1: Offline Mode (Default State)

**Expected Behavior**: System should start in offline mode, streaming offline video to Kick.

```bash
# Check container status
docker-compose ps

# Should see:
# - nginx-rtmp (healthy)
# - supervisor (running)
# - push-to-kick (running)

# Check supervisor logs
docker-compose logs supervisor | tail -20

# Should see:
# - "Starting switcher in OFFLINE mode"
# - "Switcher OFFLINE process started"

# Check nginx stats
curl -s http://localhost:8080/rtmp_stat | grep -A 5 "switch"

# Should see an active stream on /switch/out
```

## Test Scenario 2: Switch to Live Stream

**Expected Behavior**: When you publish from OBS, system should detect external IP and switch to live mode.

```bash
# Start publishing from OBS to rtmp://YOUR_IP:1936/live/mystream

# Watch supervisor logs in real-time
docker-compose logs -f supervisor

# Should see:
# - "Stream became LIVE: External publisher(s): YOUR_IP..."
# - "Stopping switcher process..."
# - "Starting switcher in LIVE mode"
# - "Mode change: offline -> live"

# Verify switcher is now pulling live stream
tail -f logs/switcher.log

# Should show FFmpeg pulling from rtmp://nginx-rtmp:1935/live/mystream
```

## Test Scenario 3: Switch Back to Offline

**Expected Behavior**: When you stop OBS, system should detect loss of external stream and switch back to offline.

```bash
# Stop streaming in OBS

# Watch supervisor logs
docker-compose logs -f supervisor

# Should see:
# - "Stream went OFFLINE: Stream not found in stats" (or similar)
# - "Stopping switcher process..."
# - "Starting switcher in OFFLINE mode"
# - "Mode change: live -> offline"
```

## Test Scenario 4: Verify Push-to-Kick Stability

**Expected Behavior**: push-to-kick should never restart during source switches.

```bash
# Get push-to-kick container start time
docker ps --format "table {{.Names}}\t{{.Status}}" | grep push-to-kick

# Perform several live/offline switches by starting and stopping OBS

# Check push-to-kick container uptime again
docker ps --format "table {{.Names}}\t{{.Status}}" | grep push-to-kick

# Container should show same uptime - it never restarted

# Check push-to-kick logs
docker-compose logs push-to-kick | tail -50

# Should show continuous connection, no restart messages
```

## Verification Commands

### Check All Logs
```bash
# View all service logs
docker-compose logs

# Filter for important events
docker-compose logs | grep -E "LIVE|OFFLINE|Starting|Mode change"

# Check individual log files
tail -f logs/switcher.log
tail -f logs/push-to-kick.log
```

### Check RTMP Stats
```bash
# Get XML stats
curl http://localhost:8080/rtmp_stat

# Pretty print with xmlstarlet (if installed)
curl -s http://localhost:8080/rtmp_stat | xmlstarlet fo

# Check specific applications
curl -s http://localhost:8080/rtmp_stat | grep -A 10 "application/live"
curl -s http://localhost:8080/rtmp_stat | grep -A 10 "application/switch"
```

### Check Container Resources
```bash
# Monitor resource usage
docker stats --no-stream

# Check specific containers
docker stats relayer-nginx-rtmp-1 relayer-supervisor-1 relayer-push-to-kick-1 --no-stream
```

## Common Issues and Solutions

### Issue: Switcher fails to start

**Symptoms**: Supervisor logs show "Failed to start switcher"

**Solutions**:
1. Check offline video path: `ls -lh $OFFLINE_VIDEO_PATH`
2. Verify supervisor can access ffmpeg: `docker exec relayer-supervisor-1 which ffmpeg`
3. Check switcher log: `tail -f logs/switcher.log`

### Issue: Push-to-kick not connecting to Kick

**Symptoms**: push-to-kick logs show connection errors

**Solutions**:
1. Verify KICK_URL and KICK_KEY in .env
2. Check if switcher is publishing: `curl -s http://localhost:8080/rtmp_stat | grep switch`
3. Review push-to-kick logs: `docker-compose logs push-to-kick`

### Issue: Supervisor not detecting live stream

**Symptoms**: OBS is streaming but supervisor stays in offline mode

**Solutions**:
1. Check if stream is reaching nginx: `curl -s http://localhost:8080/rtmp_stat | grep live`
2. Verify your IP is not private (127.x, 192.168.x, 10.x)
3. Check supervisor logic in logs: `docker-compose logs supervisor | grep "Check:"`

### Issue: Frequent switcher restarts

**Symptoms**: Switcher keeps stopping and starting

**Solutions**:
1. Check for unstable OBS connection
2. Verify network stability
3. Increase POLL_INTERVAL in .env to reduce sensitivity
4. Check supervisor logs for error patterns

## Performance Benchmarks

Expected CPU usage (1080p @ 30fps, 6Mbps):
- nginx-rtmp: ~5-10% of one core
- supervisor: ~1-2% of one core
- switcher (in supervisor): ~40-50% of one core
- push-to-kick: ~5-10% of one core (copy codec)

Expected RAM usage:
- nginx-rtmp: ~50MB
- supervisor: ~100MB
- push-to-kick: ~50MB
- Total: ~200MB + switcher overhead

## Success Criteria

✅ System starts in offline mode with switcher publishing to /switch/out
✅ push-to-kick maintains connection to Kick
✅ Supervisor detects external live stream and switches to live mode
✅ Switcher restarts with new source (live)
✅ push-to-kick continues without interruption
✅ Supervisor detects loss of live stream
✅ Switcher restarts back to offline mode
✅ push-to-kick still maintains connection (never restarted)
✅ No errors in any service logs
✅ Kick stream shows smooth transition with ~0.5s gap during switches

## Clean Up

```bash
# Stop all services
docker-compose down

# Remove all containers and networks
docker-compose down -v

# Remove images if needed
docker-compose down --rmi all