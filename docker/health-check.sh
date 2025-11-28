#!/bin/bash
# Health check script for ts-multiplexer
# Verifies that:
# 1. UDP receivers are ready and listening on ports 10000 and 10001
# 2. RTMP output is connected and actively writing data (via internal API)

set -e

# Check if UDP port is bound and listening
check_udp_port() {
    local port=$1
    # Check if the port is bound (ss shows listening UDP sockets)
    if ss -uln | grep -q ":${port} "; then
        return 0
    else
        return 1
    fi
}

# Check RTMP output health via internal HTTP API
# This is isolated within the container - no external connections needed
check_rtmp_health() {
    local timeout=2
    local response
    
    # Query the internal health endpoint
    response=$(curl -sf --max-time ${timeout} "http://localhost:8091/health" 2>/dev/null)
    
    if [ $? -ne 0 ]; then
        echo "Health API not responding"
        return 1
    fi
    
    # Check if status is "healthy" (connected and writing data)
    if echo "$response" | grep -q '"status": "healthy"'; then
        return 0
    else
        echo "RTMP output unhealthy: $response"
        return 1
    fi
}

# Check live stream port (10000)
if ! check_udp_port 10000; then
    echo "Health check failed: UDP port 10000 not bound"
    exit 1
fi

# Check fallback stream port (10001)
if ! check_udp_port 10001; then
    echo "Health check failed: UDP port 10001 not bound"
    exit 1
fi

# Check RTMP output health
if ! check_rtmp_health; then
    echo "Health check failed: RTMP output not healthy"
    exit 1
fi

# All checks passed
echo "Health check passed: UDP ports (10000, 10001) ready and RTMP output healthy"
exit 0