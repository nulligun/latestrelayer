#!/bin/bash

# Default values
DURATION=10
PATTERN="brb"
OUTPUT="videos/fallback.mp4"
FREQUENCY=""

# Usage function
usage() {
    cat << EOF
Usage: $0 [-d DURATION] [-f FREQUENCY] [-p PATTERN] [-o OUTPUT] [-h]

Generate a fallback video file with specific encoding parameters.

Options:
    -d DURATION   Duration in seconds (default: 10)
    -f FREQUENCY  Audio sine wave frequency in Hz (optional)
                  If not specified, silent audio will be generated
    -p PATTERN    Video pattern (default: brb)
                  Options:
                    brb        - Black screen with yellow "BRB..." text
                    colorbars  - PAL color bars
                    smptebars  - SMPTE HD color bars
                    testsrc    - FFmpeg test source pattern
    -o OUTPUT     Output file path (default: videos/fallback.mp4)
    -h            Show this help message

Output Specifications:
    - Resolution: 1280x720 (720p)
    - Frame rate: 30 fps
    - Video: H.264, 1500 kbps CBR, keyframe every 0.5s
    - Audio: AAC stereo, 128 kbps, silent or sine wave if -f specified

Examples:
    $0                                    # Generate 10s BRB video with silent audio
    $0 -d 30 -p colorbars -f 440         # Generate 30s color bars with 440 Hz tone
    $0 -d 60 -p smptebars -o test.mp4   # Generate 60s SMPTE bars with silent audio

EOF
    exit 0
}

# Parse command-line arguments
while getopts "d:f:p:o:h" opt; do
    case $opt in
        d) DURATION="$OPTARG" ;;
        f) FREQUENCY="$OPTARG" ;;
        p) PATTERN="$OPTARG" ;;
        o) OUTPUT="$OPTARG" ;;
        h) usage ;;
        \?) echo "Invalid option: -$OPTARG" >&2; usage ;;
    esac
done

# Validate duration
if ! [[ "$DURATION" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "Error: Duration must be a positive number" >&2
    exit 1
fi

# Validate frequency if provided
if [ -n "$FREQUENCY" ] && ! [[ "$FREQUENCY" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "Error: Frequency must be a positive number" >&2
    exit 1
fi

# Create output directory if it doesn't exist
OUTPUT_DIR=$(dirname "$OUTPUT")
if [ ! -d "$OUTPUT_DIR" ]; then
    mkdir -p "$OUTPUT_DIR"
    echo "Created directory: $OUTPUT_DIR"
fi

# Build video filter based on pattern
case "$PATTERN" in
    brb)
        VIDEO_FILTER="color=c=black:s=1280x720:r=30:d=${DURATION},format=yuv420p,drawtext=text='BRB...':fontsize=120:fontcolor=yellow:x=(w-text_w)/2:y=(h-text_h)/2"
        ;;
    colorbars)
        VIDEO_FILTER="pal75bars=s=1280x720:r=30:d=${DURATION},format=yuv420p"
        ;;
    smptebars)
        VIDEO_FILTER="smptehdbars=s=1280x720:r=30:d=${DURATION},format=yuv420p"
        ;;
    testsrc)
        VIDEO_FILTER="testsrc=s=1280x720:r=30:d=${DURATION},format=yuv420p"
        ;;
    *)
        echo "Error: Invalid pattern '$PATTERN'. Valid options: brb, colorbars, smptebars, testsrc" >&2
        exit 1
        ;;
esac

# Build audio filter based on frequency option
if [ -n "$FREQUENCY" ]; then
    AUDIO_FILTER="sine=frequency=${FREQUENCY}:sample_rate=48000:duration=${DURATION}"
    AUDIO_DESC="${FREQUENCY} Hz sine wave"
else
    AUDIO_FILTER="anullsrc=channel_layout=stereo:sample_rate=48000,atrim=0:${DURATION}"
    AUDIO_DESC="Silent"
fi

# Generate the fallback video
echo "Generating fallback video:"
echo "  Duration: ${DURATION}s"
echo "  Pattern: ${PATTERN}"
echo "  Audio: ${AUDIO_DESC}"
echo "  Output: ${OUTPUT}"
echo ""

ffmpeg -f lavfi -i "${VIDEO_FILTER}" \
    -f lavfi -i "${AUDIO_FILTER}" \
    -c:v libx264 \
    -preset medium \
    -profile:v high \
    -level 4.0 \
    -bf 0 \
    -b:v 1500k \
    -maxrate 1500k \
    -minrate 1500k \
    -bufsize 3000k \
    -g 15 \
    -keyint_min 15 \
    -sc_threshold 0 \
    -pix_fmt yuv420p \
    -c:a aac \
    -b:a 128k \
    -ac 2 \
    -ar 48000 \
    -y \
    "$OUTPUT"

# Check if ffmpeg succeeded
if [ $? -eq 0 ]; then
    echo ""
    echo "Success! Fallback video generated: $OUTPUT"
    
    # Display file info
    if command -v ffprobe &> /dev/null; then
        echo ""
        echo "File information:"
        ffprobe -hide_banner "$OUTPUT" 2>&1 | grep -E "(Duration|Stream|Video|Audio)"
    fi
else
    echo ""
    echo "Error: Failed to generate fallback video" >&2
    exit 1
fi