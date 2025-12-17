#!/bin/bash
set -e

echo "=== Mock Drone Wrapper ==="

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

# Wait for nginx-rtmp to be ready
echo "[Wrapper] Waiting for nginx-rtmp to be ready..."
sleep 5

# Start ffmpeg in background
echo "[Wrapper] Starting mock drone stream to rtmp://nginx-rtmp:1935/publish/drone..."

ffmpeg -y -nostdin -re \
  -fflags +genpts -start_at_zero \
  -f lavfi -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=880:sample_rate=48000" \
  -filter_complex "[0:v]drawtext=text='DRONE':fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf:fontsize=72:fontcolor=white:borderw=3:bordercolor=black:x=(w-text_w)/2:y=50[vout]" \
  -map "[vout]" -map 1:a \
  -vsync cfr -r 30 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -g 60 -keyint_min 60 -sc_threshold 0 \
  -b:v 3000k -minrate 3000k -maxrate 3000k -bufsize 3000k \
  -x264-params "nal-hrd=cbr:force-cfr=1" \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -af "aresample=async=1:first_pts=0" \
  -muxdelay 0 -muxpreload 0 \
  -flvflags no_duration_filesize \
  -rtmp_live live -rtmp_buffer 0 \
  -f flv "rtmp://nginx-rtmp:1935/publish/drone" &

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