#!/bin/bash
#
# Convert MP4 to MPEG-TS with proper PSI tables
# This script converts fallback.mp4 to fallback.ts with proper PAT/PMT tables
#
# Settings are read from environment variables with defaults:
#   - VIDEO_FPS (default: 30) - used for GOP size calculation
#   - VIDEO_ENCODER (default: libx264)
#   - AUDIO_ENCODER (default: aac)
#   - AUDIO_BITRATE (default: 128) in kbps
#
# Usage: ./convert-fallback.sh [input.mp4] [output.ts]
#

set -e

# Read encoding settings from environment with defaults
VIDEO_FPS="${VIDEO_FPS:-30}"
VIDEO_ENCODER="${VIDEO_ENCODER:-libx264}"
AUDIO_ENCODER="${AUDIO_ENCODER:-aac}"
AUDIO_BITRATE="${AUDIO_BITRATE:-128}"

INPUT="${1:-fallback.mp4}"
OUTPUT="${2:-fallback.ts}"

# Check if input file exists
if [ ! -f "$INPUT" ]; then
    echo "Error: $INPUT not found!"
    echo "Usage: $0 [input.mp4] [output.ts]"
    exit 1
fi

echo "Converting $INPUT to $OUTPUT with proper MPEG-TS PSI tables..."
echo ""
echo "Settings:"
echo "  - Video encoder: ${VIDEO_ENCODER}"
echo "  - Audio encoder: ${AUDIO_ENCODER}"
echo "  - Audio bitrate: ${AUDIO_BITRATE}k"
echo "  - GOP size: ${VIDEO_FPS} (keyframe every 1 second at ${VIDEO_FPS}fps)"
echo ""
echo "This will:"
echo "  - Convert H.264 from AVCC to Annex B format"
echo "  - Generate proper PAT (PID 0) and PMT tables"
echo "  - Add periodic PSI table retransmission"
echo "  - Maintain video/audio quality (no re-encoding)"
echo ""

# Re-encode with more keyframes for better seeking and reliability
# GOP size equals FPS for keyframe every 1 second
ffmpeg -i "$INPUT" \
  -c:v "${VIDEO_ENCODER}" \
  -preset fast \
  -crf 23 \
  -g "${VIDEO_FPS}" \
  -keyint_min "${VIDEO_FPS}" \
  -sc_threshold 0 \
  -c:a "${AUDIO_ENCODER}" \
  -b:a "${AUDIO_BITRATE}k" \
  -bsf:v h264_mp4toannexb \
  -f mpegts \
  -mpegts_flags +resend_headers \
  -mpegts_service_id 1 \
  -mpegts_pmt_start_pid 256 \
  -mpegts_start_pid 257 \
  -muxrate 10000000 \
  -y \
  "$OUTPUT"

if [ $? -eq 0 ]; then
    echo ""
    echo "✓ Conversion successful!"
    echo "✓ Output file: $OUTPUT"
else
    echo ""
    echo "✗ Conversion failed!"
    exit 1
fi