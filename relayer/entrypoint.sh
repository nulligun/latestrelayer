#!/bin/bash
set -e

echo "Starting Compositor Container..."
echo "SRT listening on port 1937"
echo "TCP output on port 5000"
echo ""

# Run the compositor
exec python3 /app/main.py