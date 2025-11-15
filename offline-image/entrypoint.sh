#!/bin/bash
set -e

echo "==================================="
echo "Offline Image Streamer Container"
echo "==================================="
echo "Image: ${IMAGE_PATH}"
echo "Target: tcp://${COMPOSITOR_HOST}:${COMPOSITOR_PORT}"
echo "Retry delay: ${RETRY_DELAY}s (max: ${MAX_RETRY_DELAY}s)"
echo ""

# Shutdown flag for graceful termination
shutdown_requested=false
ffmpeg_pid=""

# Cleanup function for graceful shutdown
cleanup() {
    echo ""
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] *** SIGTERM/SIGINT RECEIVED *** Starting cleanup..."
    shutdown_requested=true
    
    # Kill the specific ffmpeg process if it's running
    if [ -n "$ffmpeg_pid" ] && kill -0 "$ffmpeg_pid" 2>/dev/null; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] Terminating ffmpeg process (PID: $ffmpeg_pid)..."
        kill -TERM "$ffmpeg_pid" 2>/dev/null || true
        # Wait briefly for graceful shutdown
        sleep 0.5
        # Force kill if still running
        if kill -0 "$ffmpeg_pid" 2>/dev/null; then
            echo "[$(date '+%Y-%m-%d %H:%M:%S')] Force killing ffmpeg..."
            kill -KILL "$ffmpeg_pid" 2>/dev/null || true
        fi
    fi
    
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Shutdown complete"
    exit 0
}

# Set up signal handlers
trap cleanup SIGTERM SIGINT

echo "[$(date '+%Y-%m-%d %H:%M:%S')] Signal handlers installed (trap cleanup SIGTERM SIGINT)"

# Initialize retry delay
current_delay=${RETRY_DELAY}

# Function to stream image as video with ffmpeg (runs in background)
stream_image() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting ffmpeg stream in background..."
    
    # Run ffmpeg in background with TCP tuning for production reliability
    ffmpeg -v verbose -stats -nostdin \
        -re \
        -loop 1 -framerate 30 -i "${IMAGE_PATH}" \
        -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 \
        -r 30 \
        -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
        -c:a aac -b:a 128k -ar 48000 -ac 2 \
        -tcp_nodelay 1 \
        -send_buffer_size 2097152 \
        -f mpegts "tcp://${COMPOSITOR_HOST}:${COMPOSITOR_PORT}?timeout=5000000&buffer_size=2097152" &
    
    # Store PID for cleanup
    ffmpeg_pid=$!
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ffmpeg started with PID: $ffmpeg_pid"
    
    # Wait for ffmpeg to complete (interruptible by signals)
    wait $ffmpeg_pid
    local exit_code=$?
    
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ffmpeg exited with code: $exit_code"
    ffmpeg_pid=""
    
    return $exit_code
}

# Main loop
echo "Starting reconnection loop (press Ctrl+C or send SIGTERM to stop)..."
echo ""

while [ "$shutdown_requested" = false ]; do
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