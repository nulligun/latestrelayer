#!/bin/bash
# Health check script for ts-multiplexer
# Verifies that multiplexer has an active TCP connection to ffmpeg-rtmp-output on port 10004
# Note: Multiplexer is a TCP client that connects to ffmpeg-rtmp-output:10004

set -e

# Check if multiplexer has an established connection to port 10004
check_tcp_connection() {
    # Use ss to check for ESTABLISHED connection to port 10004
    # The multiplexer connects TO ffmpeg-rtmp-output:10004 as a client
    if ss -tan 2>/dev/null | grep ':10004 ' | grep -q 'ESTAB'; then
        return 0
    else
        return 1
    fi
}

# Check TCP connection to port 10004
if ! check_tcp_connection; then
    echo "Health check failed: No established connection to port 10004"
    exit 1
fi

# All checks passed
echo "Health check passed: Active connection to port 10004"
exit 0