#!/bin/bash
set -e

echo "Starting FFmpeg SRT listener..."
echo "Listening on: srt://0.0.0.0:9000?mode=listener"
echo "Target: rtmp://nginx-rtmp:1936/live/cam"
echo "Transcoding: HEVC -> H.264, 1080p30, 3000k bitrate"

# Wait for nginx-rtmp to be ready
sleep 5

# Listen for SRT stream, transcode to H.264, and relay to RTMP
exec ffmpeg -i "srt://0.0.0.0:9000?mode=listener" \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast \
  -b:v 3000k -maxrate 3000k -bufsize 6000k \
  -r 30 -g 60 -keyint_min 60 \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f flv rtmp://nginx-rtmp:1936/live/cam