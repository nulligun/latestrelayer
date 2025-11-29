#!/bin/bash
# Health check for ffmpeg-rtmp-live container (drone input)
# Checks if ffmpeg process is running

# Check if ffmpeg is running
if pgrep -x "ffmpeg" > /dev/null; then
    echo "ffmpeg RTMP live (drone) is running"
    exit 0
else
    echo "ffmpeg RTMP live (drone) is not running"
    exit 1
fi