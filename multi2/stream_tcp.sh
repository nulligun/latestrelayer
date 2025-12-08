#!/bin/bash

# Default values
PATTERN="smptebars"
FREQUENCY=""
TCP_PORT="9000"

# Usage function
usage() {
    cat << EOF
Usage: $0 [-f FREQUENCY] [-p PATTERN] [-P PORT] [-h]

Stream a continuous test pattern to TCP in MPEG-TS format.
FFmpeg acts as TCP server (listen mode), and clients connect to receive the stream.

Options:
    -f FREQUENCY  Audio sine wave frequency in Hz (optional)
                  If not specified, silent audio will be generated
    -p PATTERN    Video pattern (default: smptebars)
                  Options:
                    brb        - Black screen with yellow "BRB..." text
                    colorbars  - PAL color bars
                    smptebars  - SMPTE HD color bars
                    testsrc    - FFmpeg test source pattern
    -P PORT       TCP port to listen on (default: 9000)
    -h            Show this help message

Output Specifications:
    - Resolution: 1280x720 (720p)
    - Frame rate: 30 fps
    - Video: H.264, 1500 kbps CBR, keyframe every 0.5s
    - Audio: AAC stereo, 128 kbps, silent or sine wave if -f specified
    - Format: MPEG-TS over TCP (server mode)
    - Transport: TCP (reliable, no packet loss)

Examples:
    $0                              # Stream SMPTE bars with silent audio on port 9000
    $0 -p colorbars -f 440          # Stream color bars with 440 Hz tone on port 9000
    $0 -p testsrc -P 9001           # Stream test pattern on custom port 9001

Press Ctrl+C to stop streaming.

EOF
    exit 0
}

# Parse command-line arguments
while getopts "f:p:P:h" opt; do
    case $opt in
        f) FREQUENCY="$OPTARG" ;;
        p) PATTERN="$OPTARG" ;;
        P) TCP_PORT="$OPTARG" ;;
        h) usage ;;
        \?) echo "Invalid option: -$OPTARG" >&2; usage ;;
    esac
done

# Validate TCP port
if ! [[ "$TCP_PORT" =~ ^[0-9]+$ ]] || [ "$TCP_PORT" -lt 1 ] || [ "$TCP_PORT" -gt 65535 ]; then
    echo "Error: TCP port must be a number between 1 and 65535" >&2
    exit 1
fi

# Validate frequency if provided
if [ -n "$FREQUENCY" ] && ! [[ "$FREQUENCY" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "Error: Frequency must be a positive number" >&2
    exit 1
fi

# Build video filter based on pattern
case "$PATTERN" in
    brb)
        VIDEO_FILTER="color=c=black:s=1280x720:r=30,format=yuv420p,drawtext=text='BRB...':fontsize=120:fontcolor=yellow:x=(w-text_w)/2:y=(h-text_h)/2"
        ;;
    colorbars)
        VIDEO_FILTER="pal75bars=s=1280x720:r=30,format=yuv420p"
        ;;
    smptebars)
        VIDEO_FILTER="smptehdbars=s=1280x720:r=30,format=yuv420p"
        ;;
    testsrc)
        VIDEO_FILTER="testsrc=s=1280x720:r=30,format=yuv420p"
        ;;
    testsrc2)
        VIDEO_FILTER="testsrc2=s=1280x720:r=30,format=yuv420p"
        ;;
    circletestsrc)
        VIDEO_FILTER="circletestsrc=s=1280x720:r=30,format=yuv420p"
        ;;
    mandelbrot)
        VIDEO_FILTER="mandelbrot=s=1280x720:r=30,format=yuv420p"
        ;;
    *)
        echo "Error: Invalid pattern '$PATTERN'. Valid options: brb, colorbars, smptebars, testsrc" >&2
        exit 1
        ;;
esac

# Build audio filter based on frequency option
if [ -n "$FREQUENCY" ]; then
    AUDIO_FILTER="sine=frequency=${FREQUENCY}:sample_rate=48000"
    AUDIO_DESC="${FREQUENCY} Hz sine wave"
else
    AUDIO_FILTER="anullsrc=channel_layout=stereo:sample_rate=48000"
    AUDIO_DESC="Silent"
fi

# Display streaming information
echo "Starting TCP stream server:"
echo "  Pattern: ${PATTERN}"
echo "  Audio: ${AUDIO_DESC}"
echo "  TCP Port: ${TCP_PORT} (listen mode)"
echo "  Format: MPEG-TS (720p @ 30fps, H.264 + AAC)"
echo ""
echo "Clients can connect with:"
echo "  ts_tcp_splicer -duration 10 tcp://127.0.0.1:${TCP_PORT} > output.ts"
echo ""
echo "Press Ctrl+C to stop streaming..."
echo ""

# Set up trap to cleanly exit on Ctrl+C
trap 'echo ""; echo "Stream server stopped."; exit 0' INT TERM

# Infinite loop to automatically restart on client disconnect
while true; do
    echo "Waiting for client connection on port ${TCP_PORT}..."
    
    # Stream to TCP in server mode
    # Note: -re flag ensures real-time output
    # listen=1 makes FFmpeg act as TCP server
    ffmpeg -re \
        -f lavfi -i "${VIDEO_FILTER}" \
        -f lavfi -i "${AUDIO_FILTER}" \
        -c:v libx264 \
        -preset ultrafast \
        -tune zerolatency \
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
        -f mpegts \
        "tcp://127.0.0.1:${TCP_PORT}?listen=1" 2>&1 | grep -v "Broken pipe\|Error writing trailer"
    
    # Check exit status
    EXIT_CODE=$?
    echo ""
    
    # Only show error if it's not a normal disconnect
    if [ $EXIT_CODE -ne 0 ] && [ $EXIT_CODE -ne 255 ]; then
        echo "Warning: FFmpeg exited with code $EXIT_CODE" >&2
    fi
    
    echo "Client disconnected. Restarting server..."
    sleep 1  # Brief pause before restart
done
