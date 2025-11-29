#!/bin/bash
# Health check script for ffmpeg-rtmp-live container (drone input)
# Verifies that UDP traffic is being sent to multiplexer on port 10002

set -e

# Use tcpdump to check for UDP traffic on port 10002
# -i any: listen on all interfaces
# -c 1: capture exactly 1 packet then exit (exit code 0)
# udp dst port 10002: filter for UDP packets destined for port 10002
# timeout 2: kill tcpdump after 2 seconds if no packets captured
# 2>/dev/null: suppress tcpdump's verbose output

if timeout 2 tcpdump -i any -c 1 udp dst port 10002 2>/dev/null >/dev/null; then
    echo "Health check passed: UDP traffic detected on port 10002"
    exit 0
else
    echo "Health check failed: No UDP traffic detected on port 10002"
    exit 1
fi