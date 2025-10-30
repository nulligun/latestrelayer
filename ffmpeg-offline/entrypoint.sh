#!/bin/bash
set -e

echo "Starting FFmpeg offline video stream..."
echo "Source: /videos/offline.mp4"
echo "Target: rtmp://nginx-rtmp:1936/live/offline"

# Wait for nginx-rtmp to be ready
sleep 5

# Stream the offline video in a loop
exec ffmpeg -re -stream_loop -1 -i /videos/offline.mp4 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast \
  -b:v 3000k -maxrate 3000k -bufsize 6000k \
  -r 30 -g 60 -keyint_min 60 \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f flv rtmp://nginx-rtmp:1936/live/offline