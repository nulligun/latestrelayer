#!/bin/bash
# Health check script for mock-camera container
# Verifies that SRT traffic is being sent to ffmpeg-srt-live on port 1937

set -e

# Use tcpdump to check for UDP traffic on port 1937 (SRT uses UDP)
# -i any: listen on all interfaces
# -c 1: capture exactly 1 packet then exit (exit code 0)
# udp dst port 1937: filter for UDP packets destined for port 1937
# timeout 2: kill tcpdump after 2 seconds if no packets captured
# 2>/dev/null: suppress tcpdump's verbose output

if timeout 2 tcpdump -i any -c 1 udp dst port 1937 2>/dev/null >/dev/null; then
    echo "Health check passed: SRT traffic detected to port 1937"
    exit 0
else
    echo "Health check failed: No SRT traffic detected to port 1937"
    exit 1
fi