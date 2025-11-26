#!/bin/sh
# Health check script for nginx-proxy container
# Verifies that NGINX is operational by checking HTTPS connectivity on port 443

set -e

# Check if HTTPS is responding
# Use curl with -k flag to ignore self-signed certificate validation
# Tests ONLY nginx's HTTPS functionality, not backend health
# Removed -f flag so it succeeds on any response (ignores HTTP status codes)
echo "[health-check] Checking if HTTPS is responding..."
if ! curl -sk https://localhost:443 > /dev/null 2>&1; then
    echo "[health-check] FAILED: HTTPS not responding"
    exit 1
fi
echo "[health-check] ✓ HTTPS is responding"

echo "[health-check] SUCCESS: All health checks passed"
exit 0