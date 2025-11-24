#!/bin/bash
# Test script for FFmpeg-fallback connectivity fix
# This script rebuilds containers and monitors the logs to verify the fix

set -e

echo "=========================================="
echo "FFmpeg-Fallback Connectivity Test"
echo "=========================================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Step 1: Stop and remove existing containers
echo -e "${YELLOW}Step 1: Stopping existing containers...${NC}"
docker compose down
echo -e "${GREEN}✓ Containers stopped${NC}"
echo ""

# Step 2: Rebuild the multiplexer container
echo -e "${YELLOW}Step 2: Rebuilding multiplexer container with health checks...${NC}"
docker compose build --no-cache multiplexer
echo -e "${GREEN}✓ Multiplexer rebuilt${NC}"
echo ""

# Step 3: Start containers
echo -e "${YELLOW}Step 3: Starting containers...${NC}"
docker compose up -d
echo -e "${GREEN}✓ Containers started${NC}"
echo ""

# Step 4: Wait for multiplexer to be healthy
echo -e "${YELLOW}Step 4: Waiting for multiplexer to become healthy...${NC}"
echo "This may take up to 30 seconds..."

timeout=30
counter=0
while [ $counter -lt $timeout ]; do
    health_status=$(docker inspect --format='{{.State.Health.Status}}' ts-multiplexer 2>/dev/null || echo "unknown")
    
    if [ "$health_status" = "healthy" ]; then
        echo -e "${GREEN}✓ Multiplexer is healthy!${NC}"
        break
    fi
    
    echo -n "."
    sleep 1
    counter=$((counter + 1))
done
echo ""

if [ "$health_status" != "healthy" ]; then
    echo -e "${RED}✗ Multiplexer failed to become healthy within ${timeout} seconds${NC}"
    echo "Check logs with: docker logs ts-multiplexer"
    exit 1
fi

# Step 5: Check container status
echo -e "${YELLOW}Step 5: Container status:${NC}"
docker compose ps
echo ""

# Step 6: Show recent logs
echo -e "${YELLOW}Step 6: Recent logs from multiplexer:${NC}"
echo "=========================================="
docker logs --tail 50 ts-multiplexer
echo "=========================================="
echo ""

echo -e "${YELLOW}Recent logs from ffmpeg-fallback:${NC}"
echo "=========================================="
docker logs --tail 30 ffmpeg-fallback
echo "=========================================="
echo ""

# Step 7: Check UDP port bindings
echo -e "${YELLOW}Step 7: Checking UDP port bindings in multiplexer...${NC}"
docker exec ts-multiplexer ss -uln | grep -E ":(10000|10001)" || echo "Ports not yet bound"
echo ""

# Step 8: Instructions for monitoring
echo -e "${GREEN}=========================================="
echo "Test Complete!"
echo "==========================================${NC}"
echo ""
echo "To monitor logs in real-time, use:"
echo "  docker logs -f ts-multiplexer"
echo "  docker logs -f ffmpeg-fallback"
echo ""
echo "To check if packets are being received:"
echo "  docker exec ts-multiplexer tcpdump -i any port 10001 -c 10"
echo ""
echo "To verify network connectivity:"
echo "  docker exec ffmpeg-fallback ping -c 3 multiplexer"
echo ""
echo "Expected result:"
echo "  - Multiplexer should show 'Waiting for fallback stream...'"
echo "  - Then 'Fallback stream ready!' within a few seconds"
echo "  - FFmpeg-fallback should stream continuously"
echo ""
