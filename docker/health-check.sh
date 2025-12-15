#!/bin/bash
# Health check script for ts-multiplexer
# Verifies that the multiplexer process is running

set -e

# Check if the multiplexer process is running
if ! pgrep -f "multiplexer" > /dev/null; then
    echo "Health check failed: Multiplexer process not running"
    exit 1
fi

# All checks passed
echo "Health check passed: Multiplexer process is running"
exit 0
