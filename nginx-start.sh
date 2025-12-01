#!/bin/bash
set -e

# nginx-rtmp startup script with proper logging and signal handling

echo "========================================="
echo "[nginx-rtmp] Starting nginx-rtmp server"
echo "========================================="

# Set default values for environment variables
export HLS_FRAGMENT_SIZE=${HLS_FRAGMENT_SIZE:-3}

echo "[nginx-rtmp] Configuration:"
echo "[nginx-rtmp]   HLS fragment size: ${HLS_FRAGMENT_SIZE}s"
echo "[nginx-rtmp]   RTMP port: 1935"
echo "[nginx-rtmp]   HTTP/HLS port: 8080"
echo "[nginx-rtmp]   Statistics: http://localhost:8080/stat"
echo "[nginx-rtmp]   HLS streams: http://localhost:8080/hls/"
echo "========================================="

# Generate nginx.conf from template using envsubst
if [ -f /etc/nginx/nginx.conf.template ]; then
    echo "[nginx-rtmp] Generating nginx.conf from template..."
    envsubst '${HLS_FRAGMENT_SIZE}' < /etc/nginx/nginx.conf.template > /etc/nginx/nginx.conf
    echo "[nginx-rtmp] nginx.conf generated successfully"
else
    echo "[nginx-rtmp] Warning: nginx.conf.template not found, using existing nginx.conf"
fi

# Function for fast shutdown
shutdown() {
    echo ""
    echo "[nginx-rtmp] Received shutdown signal"
    echo "[nginx-rtmp] Stopping nginx immediately..."
    nginx -s stop
    
    # Wait for nginx to finish
    wait $NGINX_PID
    
    echo "[nginx-rtmp] Shutdown complete"
    exit 0
}

# Trap signals for graceful shutdown
trap shutdown SIGTERM SIGINT SIGQUIT

# Start nginx in foreground mode
echo "[nginx-rtmp] Starting nginx process..."
nginx -g 'daemon off;' &
NGINX_PID=$!

echo "[nginx-rtmp] nginx started with PID: $NGINX_PID"
echo "[nginx-rtmp] Ready to accept RTMP connections on port 1935"
echo "[nginx-rtmp] Ready to serve HLS on port 8080"
echo ""

# Wait for nginx process
wait $NGINX_PID
EXIT_CODE=$?

if [ $EXIT_CODE -ne 0 ]; then
    echo "[nginx-rtmp] nginx exited with code $EXIT_CODE"
fi

exit $EXIT_CODE