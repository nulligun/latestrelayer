#!/bin/bash
set -e

echo "==================================="
echo "Offline Image Streamer Container"
echo "==================================="
echo "Image: ${IMAGE_PATH}"
echo "Target: tcp://${COMPOSITOR_HOST}:${COMPOSITOR_PORT}"
echo "Retry delay: ${RETRY_DELAY}s (max: ${MAX_RETRY_DELAY}s)"
echo ""

# Initialize retry delay
current_delay=${RETRY_DELAY}

# Function to stream image as video with ffmpeg
stream_image() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting ffmpeg stream..."
    
    ffmpeg \
        -loop 1 -framerate 30 -i "${IMAGE_PATH}" \
        -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 \
        -r 30 \
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
    if stream_image; then
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