#!/bin/bash
set -e

echo "Starting NGINX RTMP server..."
echo "RTMP listening on port 1936"
echo "HTTP stats available on port 8080"

# Start nginx in foreground
exec nginx -g "daemon off;"