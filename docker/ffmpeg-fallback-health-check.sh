#!/bin/bash
# Health check script for ffmpeg-fallback container
# Verifies that TCP server is listening on port 10001 for multiplexer connection

set -e

# Check if TCP port 10001 is active (listening or established)
# ss -tan: show all TCP sockets (not just listening)
# grep :10001: filter for port 10001
# Note: FFmpeg's listen=1 mode closes the listening socket once connected,
#       so we check for either LISTEN or ESTAB states

if ss -tan | grep -q ':10001 '; then
    echo "Health check passed: TCP port 10001 active"
    exit 0
else
    echo "Health check failed: No TCP activity on port 10001"
    exit 1
fi