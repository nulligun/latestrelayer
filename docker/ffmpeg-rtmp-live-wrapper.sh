#!/bin/bash
set -e

echo "=== FFmpeg RTMP Live Wrapper ==="

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

# Wait for nginx-rtmp to be ready
echo "[Wrapper] Waiting for nginx-rtmp to be ready..."
sleep 5

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

# Start ffmpeg in background
# Pulls from nginx-rtmp drone publish endpoint and serves via TCP on port 10002
echo "[Wrapper] Starting ffmpeg RTMP→TCP bridge..."
echo "[Wrapper] Full command:"
echo "ffmpeg -nostdin -loglevel info -fflags +nobuffer -i 'rtmp://nginx-rtmp/publish/drone' -c copy -f mpegts -mpegts_flags +resend_headers -max_delay ${MPEGTS_MAX_DELAY} -flush_packets 1 -async 1 ${MUXRATE_PARAM} 'tcp://0.0.0.0:10002?listen=1&send_buffer_size=${TCP_SEND_BUFFER_SIZE}'"
ffmpeg -nostdin \
    -loglevel info \
    -fflags +nobuffer \
    -i 'rtmp://nginx-rtmp/publish/drone' \
    -c copy \
    -f mpegts \
    -mpegts_flags +resend_headers \
    -max_delay ${MPEGTS_MAX_DELAY} \
    -flush_packets 1 \
    -async 1 \
    ${MUXRATE_PARAM} \
    "tcp://0.0.0.0:10002?listen=1&send_buffer_size=${TCP_SEND_BUFFER_SIZE}" &

FFMPEG_PID=$!
echo "[Wrapper] FFmpeg started with PID $FFMPEG_PID"

# Wait for ffmpeg to finish
wait $FFMPEG_PID
EXIT_CODE=$?

echo "[Wrapper] FFmpeg exited with code $EXIT_CODE"
exit $EXIT_CODE