#!/bin/bash

echo "=== Mock Camera Shutdown Test ==="
echo "This test verifies that the mock-camera container shuts down quickly with proper signal handling"
echo ""

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to test shutdown time
test_shutdown() {
    local test_name="$1"
    echo -e "${YELLOW}Test: ${test_name}${NC}"
    
    # Start the container
    echo "Starting mock-camera container..."
    docker compose --profile manual up -d mock-camera
    
    # Wait for container to be running
    echo "Waiting for container to start..."
    sleep 8
    
    # Check if container is running
    if ! docker ps --filter "name=mock-camera" --format "{{.Names}}" | grep -q "mock-camera"; then
        echo -e "${RED}✗ Container failed to start${NC}"
        return 1
    fi
    
    echo "Container is running. Initiating shutdown..."
    
    # Measure shutdown time
    start_time=$(date +%s.%N)
    docker compose stop mock-camera
    end_time=$(date +%s.%N)
    
    # Calculate duration
    duration=$(echo "$end_time - $start_time" | bc)
    
    # Check exit code
    exit_code=$(docker inspect mock-camera --format='{{.State.ExitCode}}' 2>/dev/null || echo "unknown")
    
    echo ""
    echo "Results:"
    echo "  Shutdown time: ${duration}s"
    echo "  Exit code: ${exit_code}"
    echo ""
    
    # Verify results
    if (( $(echo "$duration < 3.0" | bc -l) )); then
        echo -e "${GREEN}✓ Shutdown time is acceptable (< 3s)${NC}"
    else
        echo -e "${RED}✗ Shutdown took too long (>= 3s)${NC}"
        return 1
    fi
    
    if [ "$exit_code" = "0" ]; then
        echo -e "${GREEN}✓ Clean exit (code 0)${NC}"
    else
        echo -e "${RED}✗ Unclean exit (code ${exit_code})${NC}"
        if [ "$exit_code" = "137" ]; then
            echo -e "${RED}  Exit code 137 means Docker force-killed with SIGKILL${NC}"
        fi
        return 1
    fi
    
    echo ""
    return 0
}

# Ensure we're in the right directory
if [ ! -f "docker-compose.yml" ]; then
    echo -e "${RED}Error: docker-compose.yml not found. Run this script from the project root.${NC}"
    exit 1
fi

# Clean up any existing containers
echo "Cleaning up any existing mock-camera containers..."
docker compose stop mock-camera 2>/dev/null || true
docker compose rm -f mock-camera 2>/dev/null || true

echo ""

# Run the test
if test_shutdown "Mock Camera Shutdown"; then
    echo -e "${GREEN}=== Test PASSED ===${NC}"
    exit_status=0
else
    echo -e "${RED}=== Test FAILED ===${NC}"
    exit_status=1
fi

# Cleanup
echo ""
echo "Cleaning up..."
docker compose stop mock-camera 2>/dev/null || true
docker compose rm -f mock-camera 2>/dev/null || true

exit $exit_status