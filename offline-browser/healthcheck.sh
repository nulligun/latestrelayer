#!/bin/bash

# Health check script for offline-browser container
# Returns 0 (healthy) if Xvfb, Chromium, and ffmpeg are running
# Returns 1 (unhealthy) otherwise

# Check if Xvfb process exists
if ! pgrep -x Xvfb > /dev/null; then
    echo "UNHEALTHY: Xvfb process not running"
    exit 1
fi

# Check if Chromium process exists
if ! pgrep -x chromium > /dev/null; then
    echo "UNHEALTHY: Chromium process not running"
    exit 1
fi

# Check if ffmpeg process exists
if ! pgrep -x ffmpeg > /dev/null; then
    echo "UNHEALTHY: ffmpeg process not running"
    exit 1
fi

# Get process PIDs
XVFB_PID=$(pgrep -x Xvfb)
CHROMIUM_PID=$(pgrep -x chromium)
FFMPEG_PID=$(pgrep -x ffmpeg)

# Check if ffmpeg is in running state (not zombie/defunct)
PROC_STATE=$(ps -o stat= -p ${FFMPEG_PID} 2>/dev/null | tr -d ' ')
if [ -z "$PROC_STATE" ]; then
    echo "UNHEALTHY: Cannot read ffmpeg process state"
    exit 1
fi

# Check if process is not zombie (Z) or defunct
if [[ "$PROC_STATE" == *"Z"* ]]; then
    echo "UNHEALTHY: ffmpeg process is zombie"
    exit 1
fi

# Check CPU usage to verify ffmpeg is actively encoding
CPU_USAGE=$(ps -o %cpu= -p ${FFMPEG_PID} 2>/dev/null | tr -d ' ')
if [ -z "$CPU_USAGE" ]; then
    echo "UNHEALTHY: Cannot read ffmpeg CPU usage"
    exit 1
fi

echo "HEALTHY: All processes running (Xvfb: ${XVFB_PID}, Chromium: ${CHROMIUM_PID}, ffmpeg: ${FFMPEG_PID}, CPU: ${CPU_USAGE}%, State: ${PROC_STATE})"
exit 0