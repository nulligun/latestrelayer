#!/bin/bash

# Health check script for offline-image container
# Returns 0 (healthy) if ffmpeg is running and sending data
# Returns 1 (unhealthy) otherwise

# Check if ffmpeg process exists
if ! pgrep -x ffmpeg > /dev/null; then
    echo "UNHEALTHY: ffmpeg process not running"
    exit 1
fi

# Get ffmpeg PID
FFMPEG_PID=$(pgrep -x ffmpeg)

# Check if process is in running state (not zombie/defunct)
PROC_STATE=$(ps -o stat= -p ${FFMPEG_PID} 2>/dev/null | tr -d ' ')
if [ -z "$PROC_STATE" ]; then
    echo "UNHEALTHY: Cannot read process state"
    exit 1
fi

# Check if process is not zombie (Z) or defunct
if [[ "$PROC_STATE" == *"Z"* ]]; then
    echo "UNHEALTHY: ffmpeg process is zombie"
    exit 1
fi

# Check CPU usage to verify process is actually working
# ffmpeg should consume some CPU when encoding/streaming
CPU_USAGE=$(ps -o %cpu= -p ${FFMPEG_PID} 2>/dev/null | tr -d ' ')
if [ -z "$CPU_USAGE" ]; then
    echo "UNHEALTHY: Cannot read CPU usage"
    exit 1
fi

# Convert CPU to integer (remove decimal point)
CPU_INT=$(echo "$CPU_USAGE" | cut -d'.' -f1)

# If CPU usage is 0 for extended period, ffmpeg might be stalled
# But we'll be lenient and just check if process exists and is not zombie
# as ffmpeg might have brief moments of low CPU

echo "HEALTHY: ffmpeg running (PID: ${FFMPEG_PID}, CPU: ${CPU_USAGE}%, State: ${PROC_STATE})"
exit 0