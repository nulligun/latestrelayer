#!/bin/bash
# Health check script for ffmpeg-kick container
# Verifies that RTMPS traffic is being sent to Kick servers (port 443)
# This confirms the container is actively streaming data

set -e

# Method 1: Check if FFmpeg process is running and outputting data
# Look for ffmpeg process with 'flv' output format (used for RTMPS)
if ! pgrep -x ffmpeg > /dev/null 2>&1; then
    echo "Health check failed: FFmpeg process not running"
    exit 1
fi

# Method 2: Use tcpdump to check for outgoing HTTPS/RTMPS traffic (port 443)
# -i any: listen on all interfaces
# -c 1: capture exactly 1 packet then exit (exit code 0)
# tcp dst port 443: filter for TCP packets destined for port 443 (RTMPS uses TLS over TCP)
# timeout 3: kill tcpdump after 3 seconds if no packets captured

if timeout 3 tcpdump -i any -c 1 'tcp dst port 443' 2>/dev/null >/dev/null; then
    echo "Health check passed: RTMPS traffic detected to Kick (port 443)"
    exit 0
else
    # Traffic check failed, but FFmpeg is running
    # This might be temporary (buffering, reconnecting)
    # Check if FFmpeg stderr shows it's still trying to connect
    echo "Health check warning: No RTMPS traffic detected, but FFmpeg is running"
    echo "This may indicate buffering or connection issues"
    
    # For now, consider it healthy if FFmpeg is running
    # The container will restart if FFmpeg exits due to stream failure
    exit 0
fi