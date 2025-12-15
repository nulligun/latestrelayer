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

# Wait for named pipe to be created by multiplexer
PIPE_PATH="/pipe/ts_output.pipe"
echo "[Wrapper] Waiting for named pipe at $PIPE_PATH..."
WAIT_COUNT=0
while [ ! -p "$PIPE_PATH" ]; do
    if [ $WAIT_COUNT -ge 30 ]; then
        echo "[Wrapper] ERROR: Named pipe not created after 30 seconds"
        exit 1
    fi
    echo "[Wrapper] Pipe not ready (attempt $((WAIT_COUNT+1))/30), waiting..."
    sleep 1
    WAIT_COUNT=$((WAIT_COUNT+1))
done
echo "[Wrapper] Named pipe ready!"
ls -l "$PIPE_PATH"

# RTMP URL (default to nginx-rtmp live stream)
RTMP_URL=${RTMP_URL:-rtmp://nginx-rtmp/live/stream}

echo "[Wrapper] Configuration:"
echo "  Input: Named pipe at $PIPE_PATH"
echo "  RTMP URL: ${RTMP_URL}"

# Infinite restart loop
while true; do
    # Start ffmpeg in background
    # Read from named pipe, receive MPEG-TS, publish to RTMP
    echo "[Wrapper] Starting ffmpeg RTMP output..."
    echo "[Wrapper] Full command:"
    echo "ffmpeg -nostdin -loglevel debug -stats -fflags +discardcorrupt+genpts -err_detect explode -f mpegts -analyzeduration 10000000 -probesize 10000000 -i '${PIPE_PATH}' -c copy -f flv '${RTMP_URL}'"

    ffmpeg -nostdin \
        -loglevel debug \
        -stats \
        -fflags +discardcorrupt+genpts \
        -err_detect explode \
        -f mpegts \
        -analyzeduration 10000000 \
        -probesize 10000000 \
        -i "${PIPE_PATH}" \
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