#!/bin/bash
set -e

echo "==================================="
echo "Offline Browser Streamer Container"
echo "==================================="
echo "URL: ${OFFLINE_SOURCE_URL}"
echo "Display: ${DISPLAY} (${DISPLAY_RESOLUTION})"
echo "Frame rate: ${FRAME_RATE}"
echo "Target: tcp://${COMPOSITOR_HOST}:${COMPOSITOR_PORT}"
echo "Retry delay: ${RETRY_DELAY}s (max: ${MAX_RETRY_DELAY}s)"
echo ""

# Parse resolution
IFS='x' read -r WIDTH HEIGHT <<< "$DISPLAY_RESOLUTION"

# Clean up any stale X server files from previous runs
echo "Cleaning up stale X server files..."
rm -f /tmp/.X99-lock
rm -f /tmp/.X11-unix/X99
echo "✓ Cleanup complete"

# Start Xvfb with output redirection
echo "Starting Xvfb on display ${DISPLAY}..."
Xvfb ${DISPLAY} -screen 0 ${WIDTH}x${HEIGHT}x24 -ac +extension GLX +render -noreset 2>&1 &
XVFB_PID=$!
echo "Xvfb started with PID: ${XVFB_PID}"

# Wait for X server to be ready and verify
echo "Waiting for X server socket to be ready..."
MAX_WAIT=10
WAIT_COUNT=0
while [ $WAIT_COUNT -lt $MAX_WAIT ]; do
    if [ -S "/tmp/.X11-unix/X99" ]; then
        echo "✓ X server socket found: /tmp/.X11-unix/X99"
        break
    fi
    sleep 1
    WAIT_COUNT=$((WAIT_COUNT + 1))
    echo "  Still waiting for socket... ($WAIT_COUNT/$MAX_WAIT)"
done

# Verify Xvfb is actually running
if ! kill -0 ${XVFB_PID} 2>/dev/null; then
    echo "ERROR: Xvfb process ${XVFB_PID} is not running!"
    exit 1
fi

# Test X server connection
if ! xdpyinfo -display ${DISPLAY} >/dev/null 2>&1; then
    echo "ERROR: Cannot connect to X server at ${DISPLAY}"
    echo "Xvfb process status:"
    ps aux | grep Xvfb | grep -v grep || echo "  No Xvfb process found"
    echo "Contents of /tmp/.X11-unix/:"
    ls -la /tmp/.X11-unix/ || echo "  Directory not found"
    exit 1
fi

echo "✓ X server is ready and accepting connections"

# Start PulseAudio with container-friendly configuration
echo "Starting PulseAudio..."

# Clean up any stale PulseAudio state from previous runs
echo "Cleaning up stale PulseAudio state..."
pulseaudio --kill 2>/dev/null || true
rm -rf /tmp/pulse /root/.config/pulse /root/.pulse-cookie
rm -f /tmp/pulse-* /tmp/.esd-* 2>/dev/null || true

# Create minimal PulseAudio config for containers
mkdir -p /tmp/pulse
cat > /tmp/pulse/client.conf << 'EOF'
autospawn = yes
daemon-binary = /usr/bin/pulseaudio
EOF

cat > /tmp/pulse/daemon.conf << 'EOF'
exit-idle-time = -1
flat-volumes = no
EOF

export PULSE_CONFIG_PATH=/tmp/pulse
export PULSE_STATE_PATH=/tmp/pulse
export PULSE_RUNTIME_PATH=/tmp/pulse

# Start PulseAudio daemon with explicit configuration
pulseaudio -D --exit-idle-time=-1 --log-target=stderr \
    --load="module-null-sink sink_name=dummy" \
    --load="module-native-protocol-unix"

# Wait for PulseAudio to be ready
echo "Waiting for PulseAudio to initialize..."
for i in {1..10}; do
    if pactl info &>/dev/null; then
        echo "✓ PulseAudio is running"
        break
    fi
    sleep 0.5
done

# Verify and configure default audio source
if ! pactl info &>/dev/null; then
    echo "ERROR: PulseAudio failed to start properly"
    exit 1
fi

# Set the dummy sink as default
pactl set-default-sink dummy
pactl set-default-source dummy.monitor

echo "✓ PulseAudio configured with default audio source"

# Launch Chromium in kiosk mode
echo "Launching Chromium browser..."
chromium \
    --kiosk \
    --no-sandbox \
    --disable-dev-shm-usage \
    --disable-gpu \
    --disable-software-rasterizer \
    --autoplay-policy=no-user-gesture-required \
    --disable-notifications \
    --disable-infobars \
    --no-first-run \
    --window-size=${WIDTH},${HEIGHT} \
    "${OFFLINE_SOURCE_URL}" &
CHROMIUM_PID=$!
echo "Chromium started with PID: ${CHROMIUM_PID}"

# Wait for page to load
echo "Waiting 5 seconds for page to load..."
sleep 5

# Initialize retry delay
current_delay=${RETRY_DELAY}

# Function to stream display with ffmpeg
stream_display() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting ffmpeg stream..."
    
    ffmpeg \
        -thread_queue_size 512 \
        -f x11grab -video_size ${DISPLAY_RESOLUTION} -framerate ${FRAME_RATE} \
        -use_wallclock_as_timestamps 1 -i ${DISPLAY}.0 \
        -thread_queue_size 512 \
        -f pulse -i default \
        -async 1 \
        -drop_pkts_on_overflow 0 \
        -r ${FRAME_RATE} \
        -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
        -c:a aac -b:a 128k -ar 48000 -ac 2 \
        -vsync cfr \
        -f mpegts "tcp://${COMPOSITOR_HOST}:${COMPOSITOR_PORT}"
    
    # Capture exit code
    return $?
}

# Cleanup function
cleanup() {
    echo ""
    echo "Shutting down..."
    kill -TERM ${CHROMIUM_PID} 2>/dev/null || true
    kill -TERM ${XVFB_PID} 2>/dev/null || true
    pulseaudio --kill 2>/dev/null || true
    exit 0
}

trap cleanup SIGTERM SIGINT

# Main loop
echo "Starting infinite reconnection loop..."
echo ""

while true; do
    # Attempt to stream
    if stream_display; then
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