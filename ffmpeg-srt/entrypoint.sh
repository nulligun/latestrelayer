#!/bin/bash
set -e

echo "Starting FFmpeg SRT listener..."
echo "Listening on: srt://0.0.0.0:9000?mode=listener"
echo "Target: rtmp://nginx-rtmp:1936/live/cam-raw"
echo "Note: Stream will be normalized by ffmpeg-cam-normalized before muxer"

# Wait for nginx-rtmp to be ready
sleep 5

# Listen for SRT stream and relay to RTMP with stream copy (no transcoding)
# The ffmpeg-cam-normalized service will handle normalization for GStreamer
exec ffmpeg -nostdin -loglevel info pipe:1 -nostats \
  -i "srt://0.0.0.0:9000?mode=listener" \
  -c copy \
  -f flv rtmp://nginx-rtmp:1936/live/cam-raw