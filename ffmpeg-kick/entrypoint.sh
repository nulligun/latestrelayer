#!/bin/bash
set -e

echo "Starting FFmpeg Kick pusher..."
echo "Source: rtmp://nginx-rtmp:1936/live/program"
echo "Target: ${KICK_URL}/${KICK_KEY}"

# Wait for nginx-rtmp and stream-switcher to be ready
sleep 10

# Relay the program stream to Kick with stream copy (no re-encoding)
exec ffmpeg -re -i rtmp://nginx-rtmp:1936/live/program \
  -c:v copy -c:a copy \
  -f flv "${KICK_URL}/${KICK_KEY}"