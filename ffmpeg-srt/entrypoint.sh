#!/bin/bash
set -e

echo "Starting FFmpeg SRT listener..."
echo "Listening on: srt://0.0.0.0:9000?mode=listener"
echo "Target: rtmp://nginx-rtmp:1936/live/cam"

# Wait for nginx-rtmp to be ready
sleep 5

# Listen for SRT stream and relay to RTMP
exec ffmpeg -i "srt://0.0.0.0:9000?mode=listener" \
  -c copy -f flv rtmp://nginx-rtmp:1936/live/cam