#!/bin/bash
set -e

echo "Starting FFmpeg Kick pusher..."
echo "Source: rtmp://nginx-rtmp:1936/live/program"
echo "Target: ${KICK_URL}/${KICK_KEY}"

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

# Poll for stream to be ready - WAIT INDEFINITELY until stream is available
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Polling for 'program' stream to be ready..."
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Will wait indefinitely until stream is available, publishing, and has bandwidth > 0..."
ATTEMPT=0
STREAM_READY=false

while [ "$STREAM_READY" = "false" ]; do
    ATTEMPT=$((ATTEMPT + 1))
    
    if curl -f -s http://nginx-rtmp:8080/stat > /tmp/rtmp_check.xml 2>&1; then
        # Check if program stream exists and is publishing using xmllint
        if xmllint --xpath "//stream[name='program']" /tmp/rtmp_check.xml > /dev/null 2>&1; then
            if xmllint --xpath "//stream[name='program']/publishing" /tmp/rtmp_check.xml > /dev/null 2>&1; then
                # Check if stream has bandwidth > 0 (actual video data flowing)
                BW_VIDEO=$(xmllint --xpath "//stream[name='program']/bw_video/text()" /tmp/rtmp_check.xml 2>/dev/null || echo "0")
                
                if [ "$BW_VIDEO" -gt 0 ] 2>/dev/null; then
                    # Convert bytes/sec to kbps for logging (using bash arithmetic)
                    BW_KBPS=$((BW_VIDEO * 8 / 1000))
                    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ✓ Stream 'program' is ready, publishing, and has bandwidth: ${BW_KBPS} kbps (after $ATTEMPT attempts)"
                    STREAM_READY=true
                    break
                else
                    # Stream exists and is publishing but no bandwidth yet
                    if [ $ATTEMPT -lt 30 ] || [ $((ATTEMPT % 5)) -eq 0 ]; then
                        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Stream 'program' exists and publishing but bandwidth is 0 (no video data yet)"
                    fi
                fi
            fi
        fi
    fi
    
    # Progressive backoff: 1s for first 30 attempts, then 5s to reduce log spam
    if [ $ATTEMPT -lt 30 ]; then
        INTERVAL=1
        if [ $((ATTEMPT % 5)) -eq 1 ] || [ $ATTEMPT -eq 1 ]; then
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] Waiting for stream... (attempt $ATTEMPT, checking every ${INTERVAL}s)"
        fi
    else
        INTERVAL=5
        # Log less frequently after 30 attempts
        if [ $((ATTEMPT % 5)) -eq 0 ] || [ $ATTEMPT -eq 30 ]; then
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] Still waiting for stream... (attempt $ATTEMPT, checking every ${INTERVAL}s)"
        fi
    fi
    
    sleep $INTERVAL
done

# Start FFmpeg - using exec to make it PID 1 for proper health checks
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting FFmpeg to stream to Kick..."
exec ffmpeg -nostdin -loglevel info -progress pipe:1 -nostats \
  -probesize 10M \
  -analyzeduration 5000000 \
  -rtmp_buffer 5000 \
  -i rtmp://nginx-rtmp:1936/live/program \
  -c:v copy -c:a copy \
  -rtmp_buffer 5000 \
  -f flv "${KICK_URL}/${KICK_KEY}"