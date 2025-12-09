#!/bin/bash
set -e

echo "=== FFmpeg Kick Wrapper ==="

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

# Configuration paths
CONFIG_FILE="/app/shared/kick_config.json"

# Save environment variables before config file check
ENV_KICK_URL="${KICK_URL}"
ENV_KICK_KEY="${KICK_KEY}"
KICK_URL=""
KICK_KEY=""

# Try to load from config file first
if [ -f "$CONFIG_FILE" ]; then
    echo "[Wrapper] Found config file at $CONFIG_FILE"
    # Read kickUrl and kickKey from JSON config
    if command -v jq &> /dev/null; then
        FILE_URL=$(jq -r '.kickUrl // empty' "$CONFIG_FILE" 2>/dev/null)
        FILE_KEY=$(jq -r '.kickKey // empty' "$CONFIG_FILE" 2>/dev/null)
        
        if [ -n "$FILE_URL" ] && [ -n "$FILE_KEY" ]; then
            KICK_URL="$FILE_URL"
            KICK_KEY="$FILE_KEY"
            echo "[Wrapper] Using configuration from config file"
        else
            echo "[Wrapper] Config file has empty values, falling back to environment variables"
        fi
    else
        echo "[Wrapper] jq not available, falling back to environment variables"
    fi
fi

# Fall back to environment variables if not set from config
if [ -z "$KICK_URL" ] || [ -z "$KICK_KEY" ]; then
    KICK_URL="${ENV_KICK_URL:-$KICK_STREAM_URL}"
    KICK_KEY="${ENV_KICK_KEY:-$KICK_STREAM_KEY}"
    echo "[Wrapper] Using configuration from environment variables"
fi

# Validate configuration
if [ -z "$KICK_URL" ]; then
    echo "[Wrapper] ERROR: KICK_URL not configured. Set KICK_URL env var or save config via dashboard."
    exit 1
fi

if [ -z "$KICK_KEY" ]; then
    echo "[Wrapper] ERROR: KICK_KEY not configured. Set KICK_KEY env var or save config via dashboard."
    exit 1
fi

# Build the full RTMPS URL
# The URL format for Kick is: rtmps://live.kick.com/app/{stream_key}
KICK_OUTPUT="${KICK_URL}${KICK_KEY}"

# Log configuration (mask the stream key)
MASKED_KEY="${KICK_KEY:0:4}...${KICK_KEY: -4}"
echo "[Wrapper] Kick URL: $KICK_URL"
echo "[Wrapper] Stream Key: $MASKED_KEY"
echo "[Wrapper] Full output: ${KICK_URL}****"

# RTMP source from nginx-rtmp
RTMP_SOURCE="rtmp://nginx-rtmp:1935/live/stream"
echo "[Wrapper] RTMP Source: $RTMP_SOURCE"

# Wait for nginx-rtmp to be ready and have a stream
echo "[Wrapper] Waiting for nginx-rtmp stream to be available..."
MAX_WAIT=60
WAITED=0
while [ $WAITED -lt $MAX_WAIT ]; do
    # Check if there's an active stream on nginx-rtmp
    if curl -s "http://nginx-rtmp:8080/stat" 2>/dev/null | grep -q "<name>stream</name>"; then
        echo "[Wrapper] Stream detected on nginx-rtmp"
        break
    fi
    echo "[Wrapper] Waiting for stream... ($WAITED/$MAX_WAIT seconds)"
    sleep 2
    WAITED=$((WAITED + 2))
done

if [ $WAITED -ge $MAX_WAIT ]; then
    echo "[Wrapper] WARNING: No stream detected after ${MAX_WAIT}s, starting anyway..."
fi

# Start ffmpeg with buffering and pass-through
# -rtmp_buffer: Server-side buffer in milliseconds (3000ms = 3 seconds)
# -probesize: How much data to analyze for stream info
# -analyzeduration: How long to analyze stream
# -fflags +genpts+igndts: Generate presentation timestamps, ignore decode timestamps
# -c copy: Pass-through (no re-encoding)
echo "[Wrapper] Starting ffmpeg stream to Kick..."
echo "[Wrapper] Full command:"
echo "ffmpeg -nostdin -loglevel info -rtmp_buffer 3000 -probesize 10M -analyzeduration 10M -fflags +genpts+igndts -i \"$RTMP_SOURCE\" -c copy -f flv -flvflags no_duration_filesize \"$KICK_OUTPUT\""
ffmpeg -nostdin \
    -loglevel info \
    -rtmp_buffer 3000 \
    -probesize 10M \
    -analyzeduration 10M \
    -fflags +genpts+igndts \
    -i "$RTMP_SOURCE" \
    -c copy \
    -f flv \
    -flvflags no_duration_filesize \
    "$KICK_OUTPUT" &

FFMPEG_PID=$!
echo "[Wrapper] FFmpeg started with PID $FFMPEG_PID"

# Wait for ffmpeg to finish
wait $FFMPEG_PID
EXIT_CODE=$?

echo "[Wrapper] FFmpeg exited with code $EXIT_CODE"
exit $EXIT_CODE