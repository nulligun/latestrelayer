#!/bin/bash
#
# Generate fallback video with "BRB..." message
# This script creates fallback.mp4 with the following specifications:
#   - Resolution: 1280x720 (720p)
#   - Frame rate: 30 fps
#   - Video bitrate: 1500 kbps
#   - Audio: Stereo AAC at 48 kHz with 100Hz tone
#   - Duration: 30 seconds
#   - Visual: Blue background with yellow "BRB..." text centered
#

set -e

OUTPUT="fallback.mp4"

# Check if output file already exists
if [ -f "$OUTPUT" ]; then
    echo "Warning: $OUTPUT already exists!"
    read -p "Do you want to overwrite it? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Aborted. Existing file not modified."
        exit 0
    fi
    echo "Overwriting $OUTPUT..."
fi

echo "Generating $OUTPUT with the following specifications:"
echo ""
echo "  Video:"
echo "    - Resolution: 1280x720"
echo "    - Frame rate: 30 fps"
echo "    - Bitrate: 1500 kbps"
echo "    - Codec: H.264"
echo "    - Background: Blue"
echo "    - Text: Yellow 'BRB...' centered"
echo ""
echo "  Audio:"
echo "    - Codec: AAC"
echo "    - Channels: Stereo"
echo "    - Sample rate: 48 kHz"
echo "    - Tone: 100 Hz sine wave"
echo ""
echo "  Duration: 30 seconds"
echo ""

ffmpeg -f lavfi -i color=c=blue:s=1280x720:r=30 \
  -f lavfi -i sine=frequency=100:sample_rate=48000 \
  -vf "drawtext=text='BRB...':fontcolor=yellow:fontsize=72:x=(w-text_w)/2:y=(h-text_h)/2" \
  -c:v libx264 -b:v 1500k -r 30 -pix_fmt yuv420p \
  -c:a aac -ac 2 -ar 48000 \
  -t 30 \
  -y \
  "$OUTPUT"

if [ $? -eq 0 ]; then
    echo ""
    echo "✓ Generation successful!"
    echo "✓ Output file: $OUTPUT"
    echo ""
    echo "Next steps:"
    echo "  1. Run ./convert-fallback.sh to convert to MPEG-TS format"
    echo "  2. Start the system with: docker-compose up"
else
    echo ""
    echo "✗ Generation failed!"
    echo "Make sure ffmpeg is installed and supports the required codecs."
    exit 1
fi