#!/bin/bash
# Health check script for ts-multiplexer
# Verifies that RTMP output is connected and actively writing data (via internal API)
# Note: Multiplexer is a TCP client that connects to ffmpeg containers,
# so it doesn't bind to ports 10000/10001 - those are bound by ffmpeg servers.

set -e

# Check RTMP output health via internal HTTP API
# This is isolated within the container - no external connections needed
check_rtmp_health() {
    local timeout=2
    local response
    
    # Query the internal health endpoint
    response=$(curl -sf --max-time ${timeout} "http://localhost:8091/health" 2>/dev/null)
    
    if [ $? -ne 0 ]; then
        echo "Health check failed: Health API not responding"
        return 1
    fi
    
    # Check if status is "healthy" (connected and writing data)
    if echo "$response" | grep -q '"status": "healthy"'; then
        return 0
    else
        echo "Health check failed: RTMP output unhealthy: $response"
        return 1
    fi
}

# Check RTMP output health
if ! check_rtmp_health; then
    exit 1
fi

# All checks passed
echo "Health check passed: RTMP output healthy"
exit 0