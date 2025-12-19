#!/bin/bash

echo "=== FFmpeg SRT Live Wrapper ==="

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

# Wait for multiplexer to be ready
echo "[Wrapper] Waiting for multiplexer to be ready..."
sleep 3

# Get SRT latency from environment variable (default: 80ms for low latency)
# SRT latency is specified in microseconds in the URL
SRT_LATENCY_MS=${SRT_LATENCY_MS:-80}
SRT_LATENCY_US=$((SRT_LATENCY_MS * 1000))
echo "[Wrapper] Using SRT latency: ${SRT_LATENCY_MS}ms (${SRT_LATENCY_US}µs)"

# Named pipe path
PIPE_PATH="/pipe/camera.ts"

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
    echo "[Wrapper] Starting ffmpeg SRT live stream (named pipe output)..."
    echo "[Wrapper] Full command:"
    echo "ffmpeg -y -nostdin -loglevel debug -fflags +nobuffer+flush_packets+genpts -flags low_delay -probesize 32768 -analyzeduration 0 -i \"srt://0.0.0.0:1937?mode=listener&latency=${SRT_LATENCY_US}&transtype=live&rcvbuf=1000000&payload_size=1316\" -c copy -f mpegts -mpegts_flags +resend_headers -flush_packets 1 -max_delay 0 -max_interleave_delta 0 \"${PIPE_PATH}\""
    ffmpeg -y -nostdin \
        -loglevel debug \
        -stats \
        -fflags +nobuffer+flush_packets+genpts \
        -flags low_delay \
        -probesize 32768 \
        -analyzeduration 0 \
        -i "srt://0.0.0.0:1937?mode=listener&latency=${SRT_LATENCY_US}&transtype=live&rcvbuf=1000000&payload_size=1316" \
        -c copy \
        -f mpegts \
        -mpegts_flags +resend_headers \
        -flush_packets 1 \
        -max_delay 0 \
        -max_interleave_delta 0 \
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