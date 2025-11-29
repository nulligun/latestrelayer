#!/bin/bash
#
# Generate fallback video with "BRB..." message
# This script creates fallback.mp4 with configurable encoding settings.
#
# Settings are read from environment variables with defaults:
#   - VIDEO_WIDTH (default: 1280)
#   - VIDEO_HEIGHT (default: 720)
#   - VIDEO_FPS (default: 30)
#   - VIDEO_BITRATE (default: 1500) in kbps
#   - VIDEO_ENCODER (default: libx264)
#   - AUDIO_ENCODER (default: aac)
#   - AUDIO_BITRATE (default: 128) in kbps
#   - AUDIO_SAMPLE_RATE (default: 48000) in Hz
#
# Audio is always stereo. Duration is 30 seconds.
# Visual: Black background with yellow "BRB..." text centered
#

set -e

# Read encoding settings from environment with defaults
VIDEO_WIDTH="${VIDEO_WIDTH:-1280}"
VIDEO_HEIGHT="${VIDEO_HEIGHT:-720}"
VIDEO_FPS="${VIDEO_FPS:-30}"
VIDEO_BITRATE="${VIDEO_BITRATE:-1500}"
VIDEO_ENCODER="${VIDEO_ENCODER:-libx264}"
AUDIO_ENCODER="${AUDIO_ENCODER:-aac}"
AUDIO_BITRATE="${AUDIO_BITRATE:-128}"
AUDIO_SAMPLE_RATE="${AUDIO_SAMPLE_RATE:-48000}"

OUTPUT="${1:-fallback.mp4}"

# Skip interactive confirmation when running in container/non-interactive mode
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
echo "    - Resolution: ${VIDEO_WIDTH}x${VIDEO_HEIGHT}"
echo "    - Frame rate: ${VIDEO_FPS} fps"
echo "    - Bitrate: ${VIDEO_BITRATE} kbps"
echo "    - Codec: ${VIDEO_ENCODER}"
echo "    - Background: Black"
echo "    - Text: Yellow 'BRB...' centered"
echo ""
echo "  Audio:"
echo "    - Codec: ${AUDIO_ENCODER}"
echo "    - Channels: Stereo"
echo "    - Sample rate: ${AUDIO_SAMPLE_RATE} Hz"
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
    ffmpeg -f lavfi -i "color=c=black:s=${VIDEO_WIDTH}x${VIDEO_HEIGHT}:r=${VIDEO_FPS}" \
      -f lavfi -i "anullsrc=channel_layout=stereo:sample_rate=${AUDIO_SAMPLE_RATE}" \
      -c:v "${VIDEO_ENCODER}" -b:v "${VIDEO_BITRATE}k" -r "${VIDEO_FPS}" -pix_fmt yuv420p \
      -c:a "${AUDIO_ENCODER}" -ac 2 -ar "${AUDIO_SAMPLE_RATE}" \
      -t 30 \
      -y \
      "$OUTPUT"
else
    echo "Using font: $FONTFILE"
    ffmpeg -f lavfi -i "color=c=black:s=${VIDEO_WIDTH}x${VIDEO_HEIGHT}:r=${VIDEO_FPS}" \
      -f lavfi -i "anullsrc=channel_layout=stereo:sample_rate=${AUDIO_SAMPLE_RATE}" \
      -vf "drawtext=fontfile='$FONTFILE':text='BRB...':fontcolor=yellow:fontsize=72:x=(w-text_w)/2:y=(h-text_h)/2" \
      -c:v "${VIDEO_ENCODER}" -b:v "${VIDEO_BITRATE}k" -r "${VIDEO_FPS}" -pix_fmt yuv420p \
      -c:a "${AUDIO_ENCODER}" -ac 2 -ar "${AUDIO_SAMPLE_RATE}" \
      -t 30 \
      -y \
      "$OUTPUT"
fi

if [ $? -eq 0 ]; then
    echo ""
    echo "✓ Generation successful!"
    echo "✓ Output file: $OUTPUT"
else
    echo ""
    echo "✗ Generation failed!"
    echo "Make sure ffmpeg is installed and supports the required codecs."
    exit 1
fi