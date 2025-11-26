#!/bin/bash
# Health check script for ts-multiplexer
# Verifies that:
# 1. UDP receivers are ready and listening on ports 10000 and 10001
# 2. TCP connection to nginx-rtmp server on port 1935 is established

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

# Check TCP connection to nginx-rtmp
check_rtmp_connection() {
    local host="nginx-rtmp"
    local port=1935
    local timeout=2
    
    # Use netcat to test TCP connection
    # -z : scan without sending data
    # -w : timeout in seconds
    if nc -z -w ${timeout} ${host} ${port} 2>/dev/null; then
        return 0
    else
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

# Check RTMP connection
if ! check_rtmp_connection; then
    echo "Health check failed: Cannot establish TCP connection to nginx-rtmp:1935"
    exit 1
fi

# All checks passed
echo "Health check passed: UDP ports (10000, 10001) ready and RTMP connection established"
exit 0