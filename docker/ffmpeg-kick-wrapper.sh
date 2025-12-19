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

# HTTP-FLV source from SRS (supports reconnection)
HTTP_FLV_SOURCE="http://srs:8080/live/stream.flv"
echo "[Wrapper] HTTP-FLV Source: $HTTP_FLV_SOURCE"

# Wait for srs to be ready and have a stream
echo "[Wrapper] Waiting for srs stream to be available..."
MAX_WAIT=60
WAITED=0
while [ $WAITED -lt $MAX_WAIT ]; do
    # Check if SRS API is responding
    if curl -s -o /dev/null -w "%{http_code}" "http://srs:1985/api/v1/streams/" 2>/dev/null | grep -q "200"; then
        echo "[Wrapper] SRS API is ready"
        break
    fi
    echo "[Wrapper] Waiting for SRS API... ($WAITED/$MAX_WAIT seconds)"
    sleep 2
    WAITED=$((WAITED + 2))
done

if [ $WAITED -ge $MAX_WAIT ]; then
    echo "[Wrapper] WARNING: No stream detected after ${MAX_WAIT}s, starting anyway..."
fi

# Start ffmpeg with low-latency and auto-reconnect
# Input reconnect options (work with HTTP):
#   -reconnect 1: Enable reconnection on input
#   -reconnect_streamed 1: Reconnect even after streaming started  
#   -reconnect_at_eof 1: Reconnect at end of stream
#   -reconnect_delay_max 2: Max 2s between attempts (fast recovery)
#   -reconnect_on_network_error 1: Handle network failures
#
# Low-latency options:
#   -probesize 32k: Minimal probe (known format)
#   -analyzeduration 500000: 0.5s analysis (was 10s!)
#   -fflags +nobuffer+genpts+igndts: No buffering, fix timestamps
#   -flags low_delay: Low-delay decoding mode
#
# Output options:
#   -flush_packets 1: Immediate output flush (critical for latency)
echo "[Wrapper] Starting ffmpeg stream to Kick (HTTP-FLV input, auto-reconnect)..."
echo "[Wrapper] Full command:"
echo "ffmpeg -nostdin -loglevel info -reconnect 1 -reconnect_streamed 1 -reconnect_at_eof 1 -reconnect_delay_max 2 -reconnect_on_network_error 1 -probesize 32k -analyzeduration 500000 -fflags +nobuffer+genpts+igndts -flags low_delay -i \"$HTTP_FLV_SOURCE\" -c copy -f flv -flvflags no_duration_filesize -flush_packets 1 \"$KICK_OUTPUT\""
ffmpeg -nostdin \
    -loglevel info \
    -reconnect 1 \
    -reconnect_streamed 1 \
    -reconnect_at_eof 1 \
    -reconnect_delay_max 2 \
    -reconnect_on_network_error 1 \
    -probesize 32k \
    -analyzeduration 500000 \
    -fflags +nobuffer+genpts+igndts \
    -flags low_delay \
    -i "$HTTP_FLV_SOURCE" \
    -c copy \
    -f flv \
    -flvflags no_duration_filesize \
    -flush_packets 1 \
    "$KICK_OUTPUT" &

FFMPEG_PID=$!
echo "[Wrapper] FFmpeg started with PID $FFMPEG_PID"

# Wait for ffmpeg to finish
wait $FFMPEG_PID
EXIT_CODE=$?

echo "[Wrapper] FFmpeg exited with code $EXIT_CODE"
exit $EXIT_CODE