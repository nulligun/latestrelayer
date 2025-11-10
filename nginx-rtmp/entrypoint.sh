#!/bin/bash
set -e

echo "Starting NGINX RTMP server..."
echo "RTMP will listen on port 1936"
echo "HTTP stats will be available on port 8080"

# Trap SIGTERM and SIGINT to forward to nginx process
trap 'echo "Received shutdown signal, stopping nginx..."; kill -TERM $NGINX_PID 2>/dev/null || true; wait $NGINX_PID; exit 0' SIGTERM SIGINT

# Start nginx in foreground
nginx -g "daemon off;" &
NGINX_PID=$!

# Wait for ports to be ready
echo "Waiting for NGINX to initialize..."
sleep 2

# Verify RTMP port is listening
if netstat -tln | grep -q ":1936 "; then
    echo "✓ RTMP port 1936 is listening"
else
    echo "⚠ Warning: RTMP port 1936 may not be listening yet"
fi

# Verify HTTP port is listening
if netstat -tln | grep -q ":8080 "; then
    echo "✓ HTTP stats port 8080 is listening"
else
    echo "⚠ Warning: HTTP port 8080 may not be listening yet"
fi

echo "NGINX RTMP server ready to accept connections"

# Wait for nginx process
wait $NGINX_PID