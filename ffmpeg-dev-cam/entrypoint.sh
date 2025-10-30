#!/bin/bash
set -e

echo "Starting FFmpeg dev camera stream (simulated)..."
echo "Source: /videos/offline2.mp4"
echo "Target: rtmp://nginx-rtmp:1936/live/cam"

# Wait for nginx-rtmp to be ready
sleep 5

# Stream the camera simulation video in a loop
exec ffmpeg -re -stream_loop -1 -i /videos/offline2.mp4 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast \
  -b:v 3000k -maxrate 3000k -bufsize 6000k \
  -r 30 -g 60 -keyint_min 60 \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f flv rtmp://nginx-rtmp:1936/live/cam