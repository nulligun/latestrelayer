#!/bin/bash
# Health check for ffmpeg-rtmp-input container
# Checks if FFmpeg process is running and TCP port 10002 is listening

# Check if FFmpeg process is running
if ! pgrep -x ffmpeg > /dev/null; then
    echo "FFmpeg process not found"
    exit 1
fi

# Check if TCP port 10002 is listening
if ! netstat -tln 2>/dev/null | grep -q ":10002.*LISTEN" && \
   ! ss -tln 2>/dev/null | grep -q ":10002.*LISTEN"; then
    echo "TCP port 10002 not listening"
    exit 1
fi

echo "Health check passed"
exit 0