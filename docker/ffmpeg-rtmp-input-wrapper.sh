#!/bin/bash

echo "=== FFmpeg RTMP Live Wrapper ==="

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

# Wait for srs to be ready
echo "[Wrapper] Waiting for srs to be ready..."
sleep 5

# Named pipe path
PIPE_PATH="/pipe/drone.ts"

echo "[Wrapper] Waiting for named pipe at $PIPE_PATH..."
WAIT_COUNT=0
while [ ! -p "$PIPE_PATH" ]; do
    if [ $WAIT_COUNT -ge 60 ]; then
        echo "[Wrapper] ERROR: Named pipe not created after 60 seconds"
        exit 1
    fi
    echo "[Wrapper] Pipe not ready (attempt $((WAIT_COUNT+1))/60), waiting..."
    sleep 1
    WAIT_COUNT=$((WAIT_COUNT+1))
done
echo "[Wrapper] Named pipe ready!"
ls -l "$PIPE_PATH"

# Infinite restart loop
while true; do
    # Start ffmpeg in background
    # Pulls from srs drone publish endpoint and outputs to named pipe
    echo "[Wrapper] Starting ffmpeg RTMP→pipe bridge..."
    echo "[Wrapper] Full command:"
    echo "ffmpeg -y -nostdin -loglevel debug -reconnect 1 -reconnect_streamed 1 -reconnect_at_eof 1 -reconnect_delay_max 2 -i 'rtmp://srs/publish/drone' -c copy -f mpegts -mpegts_flags +resend_headers \"${PIPE_PATH}\""
    ffmpeg -y -nostdin \
        -loglevel debug \
        -fflags +nobuffer+genpts \
        -i 'rtmp://srs/publish/drone' \
        -c copy \
        -f mpegts \
        -mpegts_flags +resend_headers \
        -flush_packets 1 \
        "${PIPE_PATH}" &

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