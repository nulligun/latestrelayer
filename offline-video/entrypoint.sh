#!/bin/bash
set -e

echo "==================================="
echo "Offline Video Streamer Container"
echo "==================================="
echo "Video: ${VIDEO_PATH}"
echo "Target: tcp://${COMPOSITOR_HOST}:${COMPOSITOR_PORT}"
echo "Retry delay: ${RETRY_DELAY}s (max: ${MAX_RETRY_DELAY}s)"
echo ""

# Initialize retry delay
current_delay=${RETRY_DELAY}

# Function to stream video with ffmpeg
stream_video() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting ffmpeg stream..."
    
    ffmpeg \
        -re -stream_loop -1 \
        -i "${VIDEO_PATH}" \
        -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
        -c:a aac -b:a 128k -ar 48000 -ac 2 \
        -f mpegts "tcp://${COMPOSITOR_HOST}:${COMPOSITOR_PORT}"
    
    # Capture exit code
    return $?
}

# Main loop
echo "Starting infinite reconnection loop..."
echo ""

while true; do
    # Attempt to stream
    if stream_video; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] ffmpeg exited normally (exit code 0)"
        # Reset delay on successful run
        current_delay=${RETRY_DELAY}
    else
        exit_code=$?
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] ERROR: ffmpeg failed with exit code ${exit_code}"
        
        # Exponential backoff
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Waiting ${current_delay}s before reconnecting..."
        sleep ${current_delay}
        
        # Double the delay for next time, up to max
        current_delay=$((current_delay * 2))
        if [ ${current_delay} -gt ${MAX_RETRY_DELAY} ]; then
            current_delay=${MAX_RETRY_DELAY}
        fi
    fi
    
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Reconnecting..."
    echo ""
done