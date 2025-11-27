#!/bin/bash
#
# Generate fallback video with "BRB..." message
# This script creates fallback.mp4 with the following specifications:
#   - Resolution: 1280x720 (720p)
#   - Frame rate: 30 fps
#   - Video bitrate: 1500 kbps
#   - Audio: Stereo AAC at 48 kHz (silent)
#   - Duration: 30 seconds
#   - Visual: Black background with yellow "BRB..." text centered
#

set -e

OUTPUT="${1:-fallback.mp4}"

# Check if running in non-interactive mode (for Docker builds)
if [ -t 0 ] && [ -f "$OUTPUT" ]; then
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
echo "    - Background: Black"
echo "    - Text: Yellow 'BRB...' centered"
echo ""
echo "  Audio:"
echo "    - Codec: AAC"
echo "    - Channels: Stereo"
echo "    - Sample rate: 48 kHz"
echo "    - Silent audio"
echo ""
echo "  Duration: 30 seconds"
echo ""

# Find a suitable font file
FONTFILE=""
for font in "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" \
            "/usr/share/fonts/dejavu/DejaVuSans.ttf" \
            "/usr/share/fonts/TTF/DejaVuSans.ttf" \
            "/System/Library/Fonts/Helvetica.ttc" \
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf"; do
    if [ -f "$font" ]; then
        FONTFILE="$font"
        break
    fi
done

if [ -z "$FONTFILE" ]; then
    echo "Warning: No suitable font found, generating video without text overlay"
    ffmpeg -f lavfi -i color=c=black:s=1280x720:r=30 \
      -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 \
      -c:v libx264 -b:v 1500k -r 30 -pix_fmt yuv420p \
      -c:a aac -ac 2 -ar 48000 \
      -t 30 \
      -y \
      "$OUTPUT"
else
    echo "Using font: $FONTFILE"
    ffmpeg -f lavfi -i color=c=black:s=1280x720:r=30 \
      -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 \
      -vf "drawtext=fontfile='$FONTFILE':text='BRB...':fontcolor=yellow:fontsize=72:x=(w-text_w)/2:y=(h-text_h)/2" \
      -c:v libx264 -b:v 1500k -r 30 -pix_fmt yuv420p \
      -c:a aac -ac 2 -ar 48000 \
      -t 30 \
      -y \
      "$OUTPUT"
fi

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