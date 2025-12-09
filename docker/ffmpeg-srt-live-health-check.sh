#!/bin/bash
# Health check script for ffmpeg-srt-live container
# Verifies that TCP server is listening on port 10000 for multiplexer connection

set -e

# Check if TCP port 10000 is listening
# ss -tln: show TCP listening sockets
# grep :10000: filter for port 10000

if ss -tln | grep -q ':10000 '; then
    echo "Health check passed: TCP server listening on port 10000"
    exit 0
else
    echo "Health check failed: TCP server not listening on port 10000"
    exit 1
fi