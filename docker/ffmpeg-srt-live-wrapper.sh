#!/bin/bash
set -e

echo "=== FFmpeg SRT Live Wrapper ==="

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

# Get SRT latency from environment variable (default: 200ms)
# SRT latency is specified in microseconds in the URL
SRT_LATENCY_MS=${SRT_LATENCY_MS:-200}
SRT_LATENCY_US=$((SRT_LATENCY_MS * 1000))
echo "[Wrapper] Using SRT latency: ${SRT_LATENCY_MS}ms (${SRT_LATENCY_US}µs)"

# Start ffmpeg in background
echo "[Wrapper] Starting ffmpeg SRT live stream (TCP output)..."
echo "[Wrapper] Full command:"
echo "ffmpeg -nostdin -loglevel info -i \"srt://0.0.0.0:1937?mode=listener&latency=${SRT_LATENCY_US}&transtype=live&payload_size=1316\" -c copy -f mpegts 'tcp://0.0.0.0:10000?listen=1'"
ffmpeg -nostdin \
    -loglevel info \
    -i "srt://0.0.0.0:1937?mode=listener&latency=${SRT_LATENCY_US}&transtype=live&payload_size=1316" \
    -c copy \
    -f mpegts 'tcp://0.0.0.0:10000?listen=1' &

FFMPEG_PID=$!
echo "[Wrapper] FFmpeg started with PID $FFMPEG_PID"

# Wait for ffmpeg to finish
wait $FFMPEG_PID
EXIT_CODE=$?

echo "[Wrapper] FFmpeg exited with code $EXIT_CODE"
exit $EXIT_CODE