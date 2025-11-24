#!/bin/bash
# Health check script for ts-multiplexer
# Verifies that UDP receivers are ready and listening on ports 10000 and 10001

set -e

# Check if both UDP ports are bound and listening
# Using ss (socket statistics) which is part of iproute2 package

check_port() {
    local port=$1
    # Check if the port is bound (ss shows listening UDP sockets)
    if ss -uln | grep -q ":${port} "; then
        return 0
    else
        return 1
    fi
}

# Check live stream port (10000)
if ! check_port 10000; then
    echo "Health check failed: UDP port 10000 not bound"
    exit 1
fi

# Check fallback stream port (10001)
if ! check_port 10001; then
    echo "Health check failed: UDP port 10001 not bound"
    exit 1
fi

# Both ports are ready
echo "Health check passed: Both UDP ports (10000, 10001) are ready"
exit 0