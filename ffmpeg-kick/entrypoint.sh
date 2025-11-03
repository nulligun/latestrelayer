#!/bin/bash
set -e

echo "Starting FFmpeg Kick pusher..."
echo "Source: rtmp://nginx-rtmp:1936/live/program"
echo "Target: ${KICK_URL}/${KICK_KEY}"

# Wait for nginx-rtmp and muxer to be ready
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Waiting 10 seconds for services..."
sleep 10

# Check if muxer health endpoint is responding
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Checking muxer health..."
if curl -f -s http://muxer:8088/health > /dev/null 2>&1; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ✓ Muxer is healthy"
else
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ⚠ Stream-switcher health check failed"
fi

# Check if nginx-rtmp stats are available
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Checking nginx-rtmp stats..."
if curl -f -s http://nginx-rtmp:8080/stat > /tmp/rtmp_stat.xml 2>&1; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ✓ NGINX RTMP stats accessible"
    
    # Check if 'program' stream exists using xmllint for proper XML parsing
    if xmllint --xpath "//stream[name='program']" /tmp/rtmp_stat.xml > /dev/null 2>&1; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] ✓ Stream 'program' found in active streams"
        
        # Verify stream has active publishers using xmllint
        if xmllint --xpath "//stream[name='program']/publishing" /tmp/rtmp_stat.xml > /dev/null 2>&1; then
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] ✓ Stream 'program' is actively publishing"
        else
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] ⚠ Stream 'program' found but not publishing yet"
        fi
    else
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] ⚠ Stream 'program' NOT FOUND in active streams"
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Active streams:"
        xmllint --xpath "//stream/name/text()" /tmp/rtmp_stat.xml 2>/dev/null | sed 's/^/  - /' || echo "  (none found)"
    fi
else
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ⚠ Could not access NGINX RTMP stats"
fi

# Poll for stream to be ready with timeout
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Polling for 'program' stream to be ready..."
MAX_WAIT=60
WAIT_COUNT=0
STREAM_READY=false

while [ $WAIT_COUNT -lt $MAX_WAIT ]; do
    if curl -f -s http://nginx-rtmp:8080/stat > /tmp/rtmp_check.xml 2>&1; then
        # Check if program stream exists and is publishing using xmllint
        if xmllint --xpath "//stream[name='program']" /tmp/rtmp_check.xml > /dev/null 2>&1; then
            if xmllint --xpath "//stream[name='program']/publishing" /tmp/rtmp_check.xml > /dev/null 2>&1; then
                echo "[$(date '+%Y-%m-%d %H:%M:%S')] ✓ Stream 'program' is ready and publishing"
                STREAM_READY=true
                break
            fi
        fi
    fi
    WAIT_COUNT=$((WAIT_COUNT + 1))
    if [ $WAIT_COUNT -lt $MAX_WAIT ]; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Waiting for stream... ($WAIT_COUNT/$MAX_WAIT)"
        sleep 1
    fi
done

if [ "$STREAM_READY" = "false" ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ⚠ WARNING: Stream not confirmed ready after ${MAX_WAIT}s, attempting to connect anyway..."
fi

# Additional stabilization delay
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Waiting 3 seconds for stream to stabilize..."
sleep 3

# Start FFmpeg - using exec to make it PID 1 for proper health checks
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting FFmpeg to stream to Kick..."
exec ffmpeg -v info -stats \
  -probesize 10M \
  -analyzeduration 5000000 \
  -rtmp_buffer 5000 \
  -i rtmp://nginx-rtmp:1936/live/program \
  -c:v copy -c:a copy \
  -rtmp_buffer 5000 \
  -f flv "${KICK_URL}/${KICK_KEY}"