#!/bin/bash
set -e

echo "Starting FFmpeg BRB (Be Right Back) video stream..."
echo "Source: /videos/offline.mp4"
echo "Target: rtmp://nginx-rtmp:1936/live/brb"

# Wait for nginx-rtmp RTMP service to be fully ready
echo "Waiting for nginx-rtmp RTMP service..."
MAX_RETRIES=30
RETRY_COUNT=0

while [ $RETRY_COUNT -lt $MAX_RETRIES ]; do
  if nc -z nginx-rtmp 1936 2>/dev/null; then
    echo "✓ RTMP port 1936 is open on nginx-rtmp"
    break
  fi
  RETRY_COUNT=$((RETRY_COUNT + 1))
  echo "Waiting for RTMP port... (attempt $RETRY_COUNT/$MAX_RETRIES)"
  sleep 1
done

if [ $RETRY_COUNT -eq $MAX_RETRIES ]; then
  echo "ERROR: RTMP port 1936 never became available on nginx-rtmp"
  exit 1
fi

# Give nginx a moment to fully initialize RTMP application
sleep 2
echo "Starting FFmpeg stream to rtmp://nginx-rtmp:1936/live/brb"

# Stream the BRB video in a loop with verbose logging
exec ffmpeg -nostdin -loglevel info -progress pipe:1 -nostats \
  -re -stream_loop -1 -i /videos/offline.mp4 \
  -c:v libx264 -pix_fmt yuv420p -preset veryfast \
  -b:v 3000k -maxrate 3000k -bufsize 6000k \
  -r 30 -g 60 -keyint_min 60 \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f flv rtmp://nginx-rtmp:1936/live/brb