#!/bin/bash

# Default values
INPUT="videos/fallback.mp4"
OUTPUT="videos/fallback.ts"

# Usage function
usage() {
    cat << EOF
Usage: $0 [-i INPUT] [-o OUTPUT] [-h]

Convert an MP4 video to MPEG-TS format with optimized settings for streaming.

Options:
    -i INPUT    Input MP4 file (default: videos/fallback.mp4)
    -o OUTPUT   Output TS file (default: videos/fallback.ts)
    -h          Show this help message

Output Specifications:
    - Format: MPEG-TS
    - Video: H.264, 1500 kbps CBR, keyframe every 0.5s (GOP=15 at 30fps)
    - Audio: AAC, 128 kbps stereo
    - Optimized for HLS/streaming with frequent keyframes
    - MPEG-TS flags for reliability and seeking

Examples:
    $0                                    # Convert default input to default output
    $0 -i custom.mp4 -o custom.ts        # Convert custom files
    $0 -i videos/fallback.mp4            # Convert to default output location

EOF
    exit 0
}

# Parse command-line arguments
while getopts "i:o:h" opt; do
    case $opt in
        i) INPUT="$OPTARG" ;;
        o) OUTPUT="$OPTARG" ;;
        h) usage ;;
        \?) echo "Invalid option: -$OPTARG" >&2; usage ;;
    esac
done

# Check if input file exists
if [ ! -f "$INPUT" ]; then
    echo "Error: Input file '$INPUT' not found" >&2
    exit 1
fi

# Create output directory if it doesn't exist
OUTPUT_DIR=$(dirname "$OUTPUT")
if [ ! -d "$OUTPUT_DIR" ]; then
    mkdir -p "$OUTPUT_DIR"
    echo "Created directory: $OUTPUT_DIR"
fi

# Convert to MPEG-TS
echo "Converting to MPEG-TS format:"
echo "  Input: ${INPUT}"
echo "  Output: ${OUTPUT}"
echo ""

# Re-encode with frequent keyframes for better seeking and reliability
# GOP size of 15 means keyframe every 0.5 seconds at 30fps
# B-frames disabled (-bf 0) to ensure monotonic DTS for seamless looping
ffmpeg -i "$INPUT" \
    -c:v libx264 \
    -preset fast \
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
    -bsf:v h264_mp4toannexb \
    -f mpegts \
    -mpegts_flags +resend_headers \
    -mpegts_service_id 1 \
    -mpegts_pmt_start_pid 256 \
    -mpegts_start_pid 257 \
    -muxrate 2000000 \
    -y \
    "$OUTPUT"

# Check if ffmpeg succeeded
if [ $? -eq 0 ]; then
    echo ""
    echo "Success! MPEG-TS file created: $OUTPUT"
    
    # Display file info
    if command -v ffprobe &> /dev/null; then
        echo ""
        echo "File information:"
        ffprobe -hide_banner "$OUTPUT" 2>&1 | grep -E "(Duration|Stream|Video|Audio)"
    fi
else
    echo ""
    echo "Error: Failed to convert to MPEG-TS" >&2
    exit 1
fi