#!/bin/bash
set -e

echo "========================================"
echo "SRT Stream Switcher - Container Starting"
echo "========================================"

# Display configuration
echo "Configuration:"
echo "  SRT Port: ${SRT_PORT:-9000}"
echo "  Fallback Video: ${FALLBACK_VIDEO:-/videos/fallback.mp4}"
echo "  Output Resolution: ${OUTPUT_WIDTH:-1920}x${OUTPUT_HEIGHT:-1080}@${OUTPUT_FPS:-30}fps"
echo "  Output Bitrate: ${OUTPUT_BITRATE:-3000}kbps"
echo "  API Port: ${API_PORT:-8088}"

# Enable GStreamer debugging if requested
if [ -n "$GST_DEBUG" ]; then
    echo "  GStreamer Debug Level: $GST_DEBUG"
    export GST_DEBUG
fi

# Check if fallback video exists
if [ ! -f "${FALLBACK_VIDEO:-/videos/fallback.mp4}" ]; then
    echo "ERROR: Fallback video not found at ${FALLBACK_VIDEO:-/videos/fallback.mp4}"
    echo "Please mount a video file to this location"
    exit 1
fi

echo "✓ Fallback video found"

# Validate video file format with ffprobe
echo "Validating video file format..."
if ffprobe -v error -show_entries stream=codec_type,codec_name -of default=noprint_wrappers=1 "${FALLBACK_VIDEO:-/videos/fallback.mp4}" > /dev/null 2>&1; then
    echo "✓ Video file format validated"
else
    echo "WARNING: Could not validate video file format with ffprobe"
    echo "Continuing anyway, but pipeline may fail if format is unsupported"
fi

echo ""
echo "Starting application..."
echo ""

# Run the Python application
exec python3 /app/switcher.py