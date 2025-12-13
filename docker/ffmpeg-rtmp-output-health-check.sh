#!/bin/bash

# Health check for ffmpeg-rtmp-output container
# Checks if FFmpeg is listening on TCP port 10004 OR has an established connection

# Check if port 10004 is listening (waiting for connection)
if ss -tln 2>/dev/null | grep -q ':10004 '; then
    echo "Health check passed: Port 10004 listening"
    exit 0
fi

# Check if port 10004 has an established connection (actively streaming)
if ss -tan 2>/dev/null | grep ':10004 ' | grep -q 'ESTAB'; then
    echo "Health check passed: Port 10004 has active connection"
    exit 0
fi

echo "FFmpeg not listening and no active connection on port 10004"
exit 1
