#!/bin/bash
# Health check script for ffmpeg-fallback container
# Checks if FFmpeg process is running

set -e

# Check if FFmpeg process is running
if ! pgrep -x ffmpeg > /dev/null; then
    echo "Health check failed: FFmpeg process not running"
    exit 1
fi

echo "Health check passed: FFmpeg process is running"
exit 0
