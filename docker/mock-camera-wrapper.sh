#!/bin/bash
set -e

echo "=== Mock Camera Wrapper ==="

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

# Wait for ffmpeg-srt-live to be ready
echo "[Wrapper] Waiting for ffmpeg-srt-live to be ready..."
sleep 5

# Start ffmpeg in background
echo "[Wrapper] Starting mock camera stream..."
ffmpeg -nostdin \
    -loglevel info \
    -re \
    -f lavfi -i testsrc=size=1280x720:rate=30 \
    -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -c:v libx264 -preset slow -tune film -b:v 2M -g 60 -keyint_min 60 \
    -c:a aac -b:a 128k \
    -f mpegts 'srt://ffmpeg-srt-live:1937?mode=caller' &

FFMPEG_PID=$!
echo "[Wrapper] FFmpeg started with PID $FFMPEG_PID"

# Wait for ffmpeg to finish
wait $FFMPEG_PID
EXIT_CODE=$?

echo "[Wrapper] FFmpeg exited with code $EXIT_CODE"

# FFmpeg exits with 255 when terminated by signal (SIGTERM), which is expected
# during graceful shutdown. Treat this as successful.
if [ $EXIT_CODE -eq 255 ] || [ $EXIT_CODE -eq 0 ]; then
    exit 0
else
    exit $EXIT_CODE
fi