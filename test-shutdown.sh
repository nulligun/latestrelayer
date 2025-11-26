#!/bin/bash
# Test script to verify multiplexer container shutdown improvements

set -e

echo "========================================"
echo "Multiplexer Shutdown Test"
echo "========================================"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test 1: Shutdown during initialization (before fallback stream ready)
echo -e "${YELLOW}Test 1: Shutdown during initialization phase${NC}"
echo "Starting multiplexer container..."
docker compose up -d multiplexer

echo "Waiting 3 seconds (during initialization)..."
sleep 3

echo "Sending SIGTERM to container..."
START_TIME=$(date +%s)
docker compose stop multiplexer
END_TIME=$(date +%s)
SHUTDOWN_TIME=$((END_TIME - START_TIME))

echo ""
if [ $SHUTDOWN_TIME -le 3 ]; then
    echo -e "${GREEN}✓ Test 1 PASSED: Shutdown took ${SHUTDOWN_TIME}s (expected ≤ 3s)${NC}"
else
    echo -e "${RED}✗ Test 1 FAILED: Shutdown took ${SHUTDOWN_TIME}s (expected ≤ 3s)${NC}"
    exit 1
fi
echo ""

# Clean up
docker compose down

# Test 2: Shutdown during normal runtime
echo -e "${YELLOW}Test 2: Shutdown during normal runtime${NC}"
echo "Starting full stack (with fallback stream)..."
docker compose up -d

echo "Waiting 15 seconds for initialization to complete..."
sleep 15

echo "Verifying multiplexer is running..."
if ! docker ps | grep -q ts-multiplexer; then
    echo -e "${RED}✗ Multiplexer container not running!${NC}"
    docker compose logs multiplexer
    exit 1
fi

echo "Sending SIGTERM to container..."
START_TIME=$(date +%s)
docker compose stop multiplexer
END_TIME=$(date +%s)
SHUTDOWN_TIME=$((END_TIME - START_TIME))

echo ""
if [ $SHUTDOWN_TIME -le 3 ]; then
    echo -e "${GREEN}✓ Test 2 PASSED: Shutdown took ${SHUTDOWN_TIME}s (expected ≤ 3s)${NC}"
else
    echo -e "${RED}✗ Test 2 FAILED: Shutdown took ${SHUTDOWN_TIME}s (expected ≤ 3s)${NC}"
    exit 1
fi
echo ""

# Clean up
docker compose down

echo "========================================"
echo -e "${GREEN}All tests passed!${NC}"
echo "========================================"
echo ""
echo "Signal handling improvements verified:"
echo "  - Initialization phase responds to SIGTERM quickly"
echo "  - Runtime operation shuts down gracefully within timeout"
echo "  - No hanging processes or delayed shutdowns"