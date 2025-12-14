#!/bin/bash

echo "=== FFmpeg RTMP Output Wrapper ==="

# Global shutdown flag
SHUTDOWN_REQUESTED=false

# Signal handler for graceful shutdown
cleanup() {
    echo "[Wrapper] Received shutdown signal, stopping ffmpeg..."
    SHUTDOWN_REQUESTED=true
    if [ -n "$FFMPEG_PID" ] && kill -0 $FFMPEG_PID 2>/dev/null; then
        kill -TERM $FFMPEG_PID
        wait $FFMPEG_PID
    fi
    echo "[Wrapper] FFmpeg stopped"
    exit 0
}

# Trap SIGTERM and SIGINT
trap cleanup SIGTERM SIGINT

# Wait for nginx-rtmp to be ready
echo "[Wrapper] Waiting for nginx-rtmp to be ready..."
until nc -z nginx-rtmp 1935; do
    echo "[Wrapper] nginx-rtmp not ready, waiting..."
    sleep 2
done
echo "[Wrapper] nginx-rtmp is ready"

# Wait for multiplexer to be ready
echo "[Wrapper] Waiting for multiplexer to be ready..."
sleep 5

# Get TCP buffer configuration from environment
TCP_RECV_BUFFER_SIZE=${TCP_RECV_BUFFER_SIZE:-2097152}  # 2MB

# RTMP URL (default to nginx-rtmp live stream)
RTMP_URL=${RTMP_URL:-rtmp://nginx-rtmp/live/stream}

echo "[Wrapper] Configuration:"
echo "  TCP listen port: 10004"
echo "  TCP receive buffer: ${TCP_RECV_BUFFER_SIZE} bytes"
echo "  RTMP URL: ${RTMP_URL}"

# Infinite restart loop
while true; do
    # Start ffmpeg in background
    # Listen on TCP 10004, receive MPEG-TS, publish to RTMP
    echo "[Wrapper] Starting ffmpeg RTMP output..."
    echo "[Wrapper] Full command:"
    echo "ffmpeg -nostdin -loglevel info -stats -f mpegts -i 'tcp://0.0.0.0:10004?listen=1&recv_buffer_size=${TCP_RECV_BUFFER_SIZE}' -c copy -f flv '${RTMP_URL}'"

    ffmpeg -nostdin \
        -loglevel info \
        -stats \
        -f mpegts \
        -i "tcp://0.0.0.0:10004?listen=1&recv_buffer_size=${TCP_RECV_BUFFER_SIZE}" \
        -c copy \
        -f flv \
        "${RTMP_URL}" &

    FFMPEG_PID=$!
    echo "[Wrapper] FFmpeg started with PID $FFMPEG_PID"

    # Wait for ffmpeg to finish
    wait $FFMPEG_PID
    EXIT_CODE=$?

    echo "[Wrapper] FFmpeg exited with code $EXIT_CODE"
    
    # Check if shutdown was requested
    if [ "$SHUTDOWN_REQUESTED" = true ]; then
        echo "[Wrapper] Shutdown requested, exiting"
        exit 0
    fi
    
    # Otherwise, restart after a short delay
    echo "[Wrapper] Restarting ffmpeg in 1 second..."
    sleep 1
done