#!/bin/bash
#
# FFmpeg Fallback Wrapper
# Streams fallback content to the multiplexer via UDP.
#
# This script checks for encoding settings changes on startup and
# regenerates fallback.ts if settings have changed since last run.
#
# Settings are read from environment variables with defaults:
#   - VIDEO_BITRATE (default: 1500) in kbps
#   - VIDEO_WIDTH (default: 1280)
#   - VIDEO_HEIGHT (default: 720)
#   - VIDEO_FPS (default: 30)
#   - VIDEO_ENCODER (default: libx264)
#   - AUDIO_ENCODER (default: aac)
#   - AUDIO_BITRATE (default: 128) in kbps
#   - AUDIO_SAMPLE_RATE (default: 48000) in Hz
#
set -e

# Read encoding settings from environment with defaults
VIDEO_BITRATE="${VIDEO_BITRATE:-1500}"
VIDEO_WIDTH="${VIDEO_WIDTH:-1280}"
VIDEO_HEIGHT="${VIDEO_HEIGHT:-720}"
VIDEO_FPS="${VIDEO_FPS:-30}"
VIDEO_ENCODER="${VIDEO_ENCODER:-libx264}"
AUDIO_ENCODER="${AUDIO_ENCODER:-aac}"
AUDIO_BITRATE="${AUDIO_BITRATE:-128}"
AUDIO_SAMPLE_RATE="${AUDIO_SAMPLE_RATE:-48000}"

ENCODING_SETTINGS_FILE="/media/encoding_settings.json"

echo "=== FFmpeg Fallback Wrapper ==="
echo "[Wrapper] Encoding settings:"
echo "  Video: ${VIDEO_WIDTH}x${VIDEO_HEIGHT} @ ${VIDEO_FPS}fps, ${VIDEO_BITRATE}kbps (${VIDEO_ENCODER})"
echo "  Audio: ${AUDIO_ENCODER} @ ${AUDIO_BITRATE}kbps, ${AUDIO_SAMPLE_RATE}Hz stereo"

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

# Function to generate current settings JSON
generate_settings_json() {
    cat << EOF
{
  "video_bitrate": ${VIDEO_BITRATE},
  "video_width": ${VIDEO_WIDTH},
  "video_height": ${VIDEO_HEIGHT},
  "video_fps": ${VIDEO_FPS},
  "video_encoder": "${VIDEO_ENCODER}",
  "audio_encoder": "${AUDIO_ENCODER}",
  "audio_bitrate": ${AUDIO_BITRATE},
  "audio_sample_rate": ${AUDIO_SAMPLE_RATE}
}
EOF
}

# Function to check if settings have changed
settings_changed() {
    if [ ! -f "$ENCODING_SETTINGS_FILE" ]; then
        echo "[Wrapper] No previous settings file found"
        return 0  # Settings changed (no previous settings)
    fi
    
    # Generate current settings and compare
    CURRENT_SETTINGS=$(generate_settings_json)
    STORED_SETTINGS=$(cat "$ENCODING_SETTINGS_FILE")
    
    if [ "$CURRENT_SETTINGS" != "$STORED_SETTINGS" ]; then
        echo "[Wrapper] Encoding settings have changed since last run"
        echo "[Wrapper] Previous: $STORED_SETTINGS"
        echo "[Wrapper] Current: $CURRENT_SETTINGS"
        return 0  # Settings changed
    fi
    
    echo "[Wrapper] Encoding settings unchanged"
    return 1  # Settings not changed
}

# Function to save current settings
save_settings() {
    echo "[Wrapper] Saving current encoding settings..."
    generate_settings_json > "$ENCODING_SETTINGS_FILE"
}

# Function to regenerate fallback video
regenerate_fallback() {
    local DEFAULT_FALLBACK="$1"
    
    echo "[Wrapper] Regenerating fallback video with current settings..."
    
    # Delete existing fallback.ts if it exists
    if [ -f "$DEFAULT_FALLBACK" ]; then
        echo "[Wrapper] Deleting existing fallback.ts..."
        rm -f "$DEFAULT_FALLBACK"
    fi
    
    # Generate fallback.mp4 first
    TEMP_MP4="/tmp/fallback.mp4"
    echo "[Wrapper] Generating fallback video..."
    if /usr/local/bin/generate-fallback.sh "$TEMP_MP4"; then
        echo "[Wrapper] Converting to MPEG-TS format..."
        if /usr/local/bin/convert-fallback.sh "$TEMP_MP4" "$DEFAULT_FALLBACK"; then
            echo "[Wrapper] Fallback.ts generated successfully"
            rm -f "$TEMP_MP4"
            # Save the settings used for this generation
            save_settings
            return 0
        else
            echo "[Wrapper] ERROR: Failed to convert fallback to TS"
            rm -f "$TEMP_MP4"
            return 1
        fi
    else
        echo "[Wrapper] ERROR: Failed to generate fallback video"
        return 1
    fi
}

# Check if default fallback.ts needs to be generated or regenerated
DEFAULT_FALLBACK="/media/fallback.ts"

if [ ! -f "$DEFAULT_FALLBACK" ]; then
    echo "[Wrapper] Default fallback.ts not found, generating..."
    if ! regenerate_fallback "$DEFAULT_FALLBACK"; then
        exit 1
    fi
elif settings_changed; then
    echo "[Wrapper] Settings changed, regenerating fallback.ts..."
    if ! regenerate_fallback "$DEFAULT_FALLBACK"; then
        exit 1
    fi
else
    echo "[Wrapper] Default fallback.ts exists at $DEFAULT_FALLBACK with current settings"
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