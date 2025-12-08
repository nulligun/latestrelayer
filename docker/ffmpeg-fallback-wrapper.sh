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

# Check if default fallback.ts exists, if not generate it
DEFAULT_FALLBACK="/media/fallback.ts"
if [ ! -f "$DEFAULT_FALLBACK" ]; then
    echo "[Wrapper] Default fallback.ts not found, generating..."
    
    # Generate fallback.mp4 first
    TEMP_MP4="/tmp/fallback.mp4"
    echo "[Wrapper] Generating fallback video..."
    if /usr/local/bin/generate-fallback.sh "$TEMP_MP4"; then
        echo "[Wrapper] Converting to MPEG-TS format..."
        if /usr/local/bin/convert-fallback.sh "$TEMP_MP4" "$DEFAULT_FALLBACK"; then
            echo "[Wrapper] Fallback.ts generated successfully"
            rm -f "$TEMP_MP4"
        else
            echo "[Wrapper] ERROR: Failed to convert fallback to TS"
            rm -f "$TEMP_MP4"
            exit 1
        fi
    else
        echo "[Wrapper] ERROR: Failed to generate fallback video"
        exit 1
    fi
else
    echo "[Wrapper] Default fallback.ts exists at $DEFAULT_FALLBACK"
fi

# Select the appropriate fallback source based on configuration
echo "[Wrapper] Selecting fallback source..."
FALLBACK_FILE=$(/usr/local/bin/fallback-source-selector.sh)

if [ -z "$FALLBACK_FILE" ]; then
    echo "[Wrapper] ERROR: Could not determine fallback file, using default"
    FALLBACK_FILE="$DEFAULT_FALLBACK"
fi

# Verify the file exists
if [ ! -f "$FALLBACK_FILE" ]; then
    echo "[Wrapper] ERROR: Fallback file not found: $FALLBACK_FILE"
    echo "[Wrapper] Falling back to default: $DEFAULT_FALLBACK"
    FALLBACK_FILE="$DEFAULT_FALLBACK"
fi

echo "[Wrapper] Using fallback file: $FALLBACK_FILE"

# Start ffmpeg in background
echo "[Wrapper] Starting ffmpeg fallback stream (TCP output)..."
ffmpeg -nostdin \
    -loglevel info \
    -stats \
    -re \
    -stream_loop -1 \
    -fflags +genpts \
    -i "$FALLBACK_FILE" \
    -c copy \
    -f mpegts 'tcp://0.0.0.0:10001?listen=1' &

FFMPEG_PID=$!
echo "[Wrapper] FFmpeg started with PID $FFMPEG_PID"

# Wait for ffmpeg to finish
wait $FFMPEG_PID
EXIT_CODE=$?

echo "[Wrapper] FFmpeg exited with code $EXIT_CODE"
exit $EXIT_CODE