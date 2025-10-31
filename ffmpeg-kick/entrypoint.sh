#!/bin/bash
set -e

echo "Starting FFmpeg Kick pusher..."
echo "Source: rtmp://nginx-rtmp:1936/live/program"
echo "Target: ${KICK_URL}/${KICK_KEY}"

# Wait for nginx-rtmp and stream-switcher to be ready
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Waiting 10 seconds for services..."
sleep 10

# Check if stream-switcher health endpoint is responding
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Checking stream-switcher health..."
if curl -f -s http://stream-switcher:8088/health > /dev/null 2>&1; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ✓ Stream-switcher is healthy"
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

# Start FFmpeg with retry logic
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting FFmpeg with retry logic..."
MAX_RETRIES=5
RETRY_COUNT=0

while [ $RETRY_COUNT -lt $MAX_RETRIES ]; do
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] FFmpeg attempt $((RETRY_COUNT + 1))/$MAX_RETRIES"
    
    # Use exec only on last attempt to replace the shell process
    if [ $RETRY_COUNT -eq $((MAX_RETRIES - 1)) ]; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Final attempt - executing FFmpeg with verbose logging..."
        exec ffmpeg -v info -stats \
          -probesize 10M \
          -analyzeduration 5000000 \
          -rtmp_buffer 5000 \
          -i rtmp://nginx-rtmp:1936/live/program \
          -c:v copy -c:a copy \
          -rtmp_buffer 5000 \
          -f flv "${KICK_URL}/${KICK_KEY}"
    else
        # Run without exec so we can retry - using info level to see frame drops
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting FFmpeg with verbose logging to monitor performance..."
        ffmpeg -v info -stats \
          -probesize 10M \
          -analyzeduration 5000000 \
          -rtmp_buffer 5000 \
          -i rtmp://nginx-rtmp:1936/live/program \
          -c:v copy -c:a copy \
          -rtmp_buffer 5000 \
          -f flv "${KICK_URL}/${KICK_KEY}"
        
        EXIT_CODE=$?
        if [ $EXIT_CODE -eq 0 ]; then
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] FFmpeg exited successfully"
            exit 0
        else
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] FFmpeg failed with exit code $EXIT_CODE"
            RETRY_COUNT=$((RETRY_COUNT + 1))
            if [ $RETRY_COUNT -lt $MAX_RETRIES ]; then
                SLEEP_TIME=$((2 ** RETRY_COUNT))
                echo "[$(date '+%Y-%m-%d %H:%M:%S')] Retrying in ${SLEEP_TIME}s..."
                sleep $SLEEP_TIME
            fi
        fi
    fi
done

echo "[$(date '+%Y-%m-%d %H:%M:%S')] ERROR: All FFmpeg retry attempts failed"
exit 1