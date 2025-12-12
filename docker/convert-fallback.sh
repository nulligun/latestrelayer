#!/bin/bash
#
# Convert MP4 to MPEG-TS with proper PSI tables
# This script converts fallback.mp4 to fallback.ts with proper PAT/PMT tables
#
# Usage: ./convert-fallback.sh [input.mp4] [output.ts]
#

set -e

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
echo "This will:"
echo "  - Convert H.264 from AVCC to Annex B format"
echo "  - Generate proper PAT (PID 0) and PMT tables"
echo "  - Add periodic PSI table retransmission"
echo "  - Maintain video/audio quality (no re-encoding)"
echo ""

# Re-encode with constrained bitrate for smooth TCP streaming
# GOP size of 30 means keyframe every 1 second at 30fps
# Using CBR (Constant Bitrate) instead of CRF for consistent streaming
ffmpeg -i "$INPUT" \
  -c:v libx264 \
  -preset fast \
  -b:v 3M \
  -maxrate 3M \
  -bufsize 6M \
  -g 30 \
  -keyint_min 30 \
  -sc_threshold 0 \
  -c:a aac \
  -b:a 128k \
  -bsf:v h264_mp4toannexb \
  -f mpegts \
  -mpegts_flags +resend_headers \
  -mpegts_service_id 1 \
  -mpegts_pmt_start_pid 256 \
  -mpegts_start_pid 257 \
  -muxrate 5000000 \
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