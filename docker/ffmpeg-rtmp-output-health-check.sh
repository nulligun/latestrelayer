#!/bin/bash

# Health check for ffmpeg-rtmp-output container
# Checks if FFmpeg is listening on TCP port 10004

# Check if port 10004 is listening
if netstat -tln 2>/dev/null | grep -q ':10004 '; then
    exit 0
elif ss -tln 2>/dev/null | grep -q ':10004 '; then
    exit 0
else
    echo "FFmpeg not listening on port 10004"
    exit 1
fi