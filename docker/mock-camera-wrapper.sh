#!/bin/bash
#
# Mock Camera Wrapper
# Generates a test video stream with configurable encoding settings.
#
# Settings are read from environment variables with defaults:
#   - VIDEO_WIDTH (default: 1280)
#   - VIDEO_HEIGHT (default: 720)
#   - VIDEO_FPS (default: 30)
#   - VIDEO_ENCODER (default: libx264)
#   - AUDIO_ENCODER (default: aac)
#   - AUDIO_BITRATE (default: 128) in kbps
#   - AUDIO_SAMPLE_RATE (default: 48000) in Hz
#
# Audio is always stereo.
#
set -e

# Read encoding settings from environment with defaults
VIDEO_WIDTH="${VIDEO_WIDTH:-1280}"
VIDEO_HEIGHT="${VIDEO_HEIGHT:-720}"
VIDEO_FPS="${VIDEO_FPS:-30}"
VIDEO_ENCODER="${VIDEO_ENCODER:-libx264}"
AUDIO_ENCODER="${AUDIO_ENCODER:-aac}"
AUDIO_BITRATE="${AUDIO_BITRATE:-128}"
AUDIO_SAMPLE_RATE="${AUDIO_SAMPLE_RATE:-48000}"

echo "=== Mock Camera Wrapper ==="
echo "[Wrapper] Encoding settings:"
echo "  Video: ${VIDEO_WIDTH}x${VIDEO_HEIGHT} @ ${VIDEO_FPS}fps (${VIDEO_ENCODER})"
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

# Wait for ffmpeg-srt-live to be ready
echo "[Wrapper] Waiting for ffmpeg-srt-live to be ready..."
sleep 5

# Start ffmpeg in background
echo "[Wrapper] Starting mock camera stream..."

ffmpeg -re \
  -f lavfi -i "testsrc2=size=${VIDEO_WIDTH}x${VIDEO_HEIGHT}:rate=${VIDEO_FPS}" \
  -f lavfi -i "sine=frequency=440:sample_rate=${AUDIO_SAMPLE_RATE}" \
  -filter_complex "[1:a]aformat=channel_layouts=stereo[aout]" \
  -map 0:v -map "[aout]" \
  -c:v "${VIDEO_ENCODER}" -tune zerolatency -preset veryfast -pix_fmt yuv420p \
  -c:a "${AUDIO_ENCODER}" -b:a "${AUDIO_BITRATE}k" \
  -f mpegts "srt://ffmpeg-srt-live:1937?mode=caller" &

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