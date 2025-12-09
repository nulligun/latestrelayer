#!/bin/bash
# Health check script for ffmpeg-fallback container
# Verifies that TCP server is listening on port 10001 for multiplexer connection

set -e

# Check if TCP port 10001 is listening
# ss -tln: show TCP listening sockets
# grep :10001: filter for port 10001

if ss -tln | grep -q ':10001 '; then
    echo "Health check passed: TCP server listening on port 10001"
    exit 0
else
    echo "Health check failed: TCP server not listening on port 10001"
    exit 1
fi