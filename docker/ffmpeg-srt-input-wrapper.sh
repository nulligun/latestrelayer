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

# Get SRT latency from environment variable (default: 200ms)
# SRT latency is specified in microseconds in the URL
SRT_LATENCY_MS=${SRT_LATENCY_MS:-200}
SRT_LATENCY_US=$((SRT_LATENCY_MS * 1000))
echo "[Wrapper] Using SRT latency: ${SRT_LATENCY_MS}ms (${SRT_LATENCY_US}µs)"

# Get TCP buffering parameters from environment variables
TCP_SEND_BUFFER_SIZE=${TCP_SEND_BUFFER_SIZE:-2097152}
MPEGTS_MAX_DELAY=${MPEGTS_MAX_DELAY:-1000000}
MPEGTS_MUXRATE=${MPEGTS_MUXRATE:-5M}

# Build muxrate parameter if set
MUXRATE_PARAM=""
if [ -n "$MPEGTS_MUXRATE" ]; then
    MUXRATE_PARAM="-muxrate ${MPEGTS_MUXRATE}"
    echo "[Wrapper] TCP buffer settings: send_buffer=${TCP_SEND_BUFFER_SIZE}, max_delay=${MPEGTS_MAX_DELAY}µs, muxrate=${MPEGTS_MUXRATE}"
else
    echo "[Wrapper] TCP buffer settings: send_buffer=${TCP_SEND_BUFFER_SIZE}, max_delay=${MPEGTS_MAX_DELAY}µs, muxrate=disabled (natural bitrate)"
fi

# Infinite restart loop
while true; do
    # Start ffmpeg in background
    echo "[Wrapper] Starting ffmpeg SRT live stream (TCP output)..."
    echo "[Wrapper] Full command:"
    echo "ffmpeg -nostdin -loglevel info -fflags +nobuffer -i \"srt://0.0.0.0:1937?mode=listener&latency=${SRT_LATENCY_US}&transtype=live&payload_size=1316\" -c copy -f mpegts -mpegts_flags +resend_headers -max_delay ${MPEGTS_MAX_DELAY} -flush_packets 1 -async 1 ${MUXRATE_PARAM} 'tcp://0.0.0.0:10000?listen=1&send_buffer_size=${TCP_SEND_BUFFER_SIZE}'"
    ffmpeg -nostdin \
        -loglevel info \
        -fflags +nobuffer \
        -i "srt://0.0.0.0:1937?mode=listener&latency=${SRT_LATENCY_US}&transtype=live&payload_size=1316" \
        -c copy \
        -f mpegts \
        -mpegts_flags +resend_headers \
        -max_delay ${MPEGTS_MAX_DELAY} \
        -flush_packets 1 \
        -async 1 \
        ${MUXRATE_PARAM} \
        "tcp://0.0.0.0:10000?listen=1&send_buffer_size=${TCP_SEND_BUFFER_SIZE}" &

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