#!/bin/bash
set -e

echo "=== Mock Camera Wrapper ==="

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

# Wait for ffmpeg-srt-input to be ready
echo "[Wrapper] Waiting for ffmpeg-srt-input to be ready..."
sleep 5

# Start ffmpeg in background
echo "[Wrapper] Starting mock camera stream..."

ffmpeg -re \
  -f lavfi -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000" \
  -filter_complex "[0:v]drawtext=text='CAMERA':fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf:fontsize=72:fontcolor=white:borderw=3:bordercolor=black:x=(w-text_w)/2:y=50[vout];[1:a]aformat=channel_layouts=stereo[aout]" \
  -map "[vout]" -map "[aout]" \
  -c:v libx264 -tune zerolatency -preset veryfast -pix_fmt yuv420p \
  -g 30 -keyint_min 30 \
  -c:a aac -b:a 128k \
  -f mpegts "srt://ffmpeg-srt-input:1937?mode=caller" &

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