#!/bin/bash
# Health check script for nginx-rtmp
# Verifies that the nginx-rtmp server is responding by querying the /stat endpoint

set -e

# Configuration
STATS_URL="http://localhost:8080/stat"
TIMEOUT=3

# Check if nginx-rtmp stats endpoint is responding
if curl -f -s -m ${TIMEOUT} "${STATS_URL}" > /dev/null 2>&1; then
    echo "Health check passed: nginx-rtmp stats endpoint responding"
    exit 0
else
    echo "Health check failed: nginx-rtmp stats endpoint not responding at ${STATS_URL}"
    exit 1
fi