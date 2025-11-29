#!/bin/bash
set -e

echo "=== FFmpeg RTMP Live Wrapper (Drone Input) ==="

# Signal handler for graceful shutdown
cleanup() {
    echo "[Wrapper] Received shutdown signal, stopping ffmpeg..."
    if [ -n "$FFMPEG_PID" ] && kill -0 $FFMPEG_PID 2>/dev/null; then
        kill -TERM $FFMPEG_PID
        wait $FFMPEG_PID
    fi
    echo "[Wrapper] FFmpeg stopped"
    exit 0
}

# Trap SIGTERM and SIGINT
trap cleanup SIGTERM SIGINT

# Wait for multiplexer to be ready
echo "[Wrapper] Waiting for multiplexer to be ready..."
sleep 3

# Start ffmpeg in background
# Listen for RTMP connections on port 1938 with stream key /live/drone
# Output to UDP port 10002 on the multiplexer
echo "[Wrapper] Starting ffmpeg RTMP live stream (drone input)..."
ffmpeg -nostdin \
    -loglevel info \
    -listen 1 \
    -i 'rtmp://0.0.0.0:1938/live/drone' \
    -c copy \
    -f mpegts 'udp://multiplexer:10002?pkt_size=1316' &

FFMPEG_PID=$!
echo "[Wrapper] FFmpeg started with PID $FFMPEG_PID"

# Wait for ffmpeg to finish
wait $FFMPEG_PID
EXIT_CODE=$?

echo "[Wrapper] FFmpeg exited with code $EXIT_CODE"
exit $EXIT_CODE