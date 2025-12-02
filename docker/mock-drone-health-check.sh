#!/bin/bash
# Health check script for mock-drone container
# Verifies that RTMP traffic is being sent to nginx-rtmp on port 1935

set -e

# Use tcpdump to check for TCP traffic on port 1935 (RTMP uses TCP)
# -i any: listen on all interfaces
# -c 1: capture exactly 1 packet then exit (exit code 0)
# tcp dst port 1935: filter for TCP packets destined for port 1935
# timeout 2: kill tcpdump after 2 seconds if no packets captured
# 2>/dev/null: suppress tcpdump's verbose output

if timeout 2 tcpdump -i any -c 1 tcp dst port 1935 2>/dev/null >/dev/null; then
    echo "Health check passed: RTMP traffic detected to port 1935"
    exit 0
else
    echo "Health check failed: No RTMP traffic detected to port 1935"
    exit 1
fi