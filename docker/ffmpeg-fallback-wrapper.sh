#!/bin/bash
set -e

echo "=== FFmpeg Fallback Wrapper ==="

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
sleep 5

# Select the appropriate fallback source based on configuration
echo "[Wrapper] Selecting fallback source..."
FALLBACK_FILE=$(/usr/local/bin/fallback-source-selector.sh)

if [ -z "$FALLBACK_FILE" ]; then
    echo "[Wrapper] ERROR: Could not determine fallback file, using default"
    FALLBACK_FILE="/media/fallback.ts"
fi

# Verify the file exists
if [ ! -f "$FALLBACK_FILE" ]; then
    echo "[Wrapper] ERROR: Fallback file not found: $FALLBACK_FILE"
    echo "[Wrapper] Falling back to default: /media/fallback.ts"
    FALLBACK_FILE="/media/fallback.ts"
fi

echo "[Wrapper] Using fallback file: $FALLBACK_FILE"

# Start ffmpeg in background
echo "[Wrapper] Starting ffmpeg fallback stream..."
ffmpeg -nostdin \
    -loglevel info \
    -stats \
    -re \
    -stream_loop -1 \
    -fflags +genpts \
    -i "$FALLBACK_FILE" \
    -c copy \
    -f mpegts 'udp://multiplexer:10001?pkt_size=1316' &

FFMPEG_PID=$!
echo "[Wrapper] FFmpeg started with PID $FFMPEG_PID"

# Wait for ffmpeg to finish
wait $FFMPEG_PID
EXIT_CODE=$?

echo "[Wrapper] FFmpeg exited with code $EXIT_CODE"
exit $EXIT_CODE