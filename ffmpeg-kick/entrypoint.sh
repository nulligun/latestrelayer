#!/bin/bash
set -e

echo "==================================="
echo "Kick Streaming Container"
echo "==================================="
echo "Config: ${CONFIG_PATH}"
echo "Source: tcp://${COMPOSITOR_HOST}:${COMPOSITOR_PORT}"
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

# Try to load configuration from config file first
CONFIG_KICK_URL=""
CONFIG_KICK_KEY=""

if [ -f "${CONFIG_PATH}" ]; then
    echo "Config file found, attempting to read..."
    CONFIG_KICK_URL=$(cat "${CONFIG_PATH}" | grep -o '"kickUrl":"[^"]*"' | cut -d'"' -f4)
    CONFIG_KICK_KEY=$(cat "${CONFIG_PATH}" | grep -o '"kickKey":"[^"]*"' | cut -d'"' -f4)
    
    if [ -n "${CONFIG_KICK_URL}" ] && [ -n "${CONFIG_KICK_KEY}" ]; then
        echo "Configuration loaded from config file"
        KICK_URL="${CONFIG_KICK_URL}"
        KICK_KEY="${CONFIG_KICK_KEY}"
    else
        echo "Config file exists but values are empty, falling back to environment variables..."
    fi
else
    echo "Config file not found at ${CONFIG_PATH}, falling back to environment variables..."
fi

# Fall back to environment variables if config file didn't provide values
if [ -z "${KICK_URL}" ] || [ -z "${KICK_KEY}" ]; then
    echo "Using environment variables KICK_URL and KICK_KEY"
    KICK_URL="${KICK_URL:-}"
    KICK_KEY="${KICK_KEY:-}"
fi

# Final validation - exit only if both sources failed
if [ -z "${KICK_URL}" ] || [ -z "${KICK_KEY}" ]; then
    echo "ERROR: No Kick credentials found!"
    echo "Please either:"
    echo "  1. Configure credentials in the dashboard, OR"
    echo "  2. Set KICK_URL and KICK_KEY environment variables"
    exit 1
fi

echo "Configuration loaded successfully"
echo "Kick URL: ${KICK_URL}"
echo "Stream Key: [REDACTED]"
echo ""

# Initialize retry delay
current_delay=${RETRY_DELAY}

# Function to stream to Kick (runs in background)
stream_to_kick() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting ffmpeg stream to Kick in background..."
    
    # Stream from compositor TCP port to Kick using RTMPS
    # -re: Read input at native frame rate
    # -i: Input from compositor TCP stream
    # -c:v copy: Copy video codec (no re-encoding)
    # -c:a copy: Copy audio codec (no re-encoding)
    # -f flv: Output format FLV for RTMP
    ffmpeg -nostdin \
        -re \
        -i "tcp://${COMPOSITOR_HOST}:${COMPOSITOR_PORT}" \
        -c:v copy \
        -c:a copy \
        -f flv "${KICK_URL}/${KICK_KEY}" &
    
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
    if stream_to_kick; then
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