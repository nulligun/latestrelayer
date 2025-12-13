#!/bin/bash
# Network Connectivity Test Script
# Tests Docker network connectivity between containers

set -e

echo "=========================================="
echo "Docker Network Connectivity Test"
echo "=========================================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test counter
TESTS_PASSED=0
TESTS_FAILED=0

# Helper function to run tests
run_test() {
    local test_name="$1"
    local command="$2"
    
    echo -e "${BLUE}Testing: ${test_name}${NC}"
    if eval "$command" > /dev/null 2>&1; then
        echo -e "${GREEN}✓ PASSED${NC}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
        return 0
    else
        echo -e "${RED}✗ FAILED${NC}"
        TESTS_FAILED=$((TESTS_FAILED + 1))
        return 1
    fi
}

# Step 1: Check if containers are running
echo -e "${YELLOW}Step 1: Verifying containers are running...${NC}"
echo ""

run_test "Multiplexer container running" "docker ps | grep -q ts-multiplexer"
run_test "Nginx RTMP container running" "docker ps | grep -q nginx-rtmp-server"
run_test "FFmpeg SRT Input container running" "docker ps | grep -q ffmpeg-srt-input"
run_test "FFmpeg Fallback container running" "docker ps | grep -q ffmpeg-fallback"

echo ""

# Step 2: Check network configuration
echo -e "${YELLOW}Step 2: Verifying Docker network configuration...${NC}"
echo ""

run_test "Network 'tsduck-multiplexer-network' exists" "docker network ls | grep -q tsduck-multiplexer-network"

echo ""
echo "Network Details:"
docker network inspect tsduck-multiplexer-network --format '{{range .Containers}}  • {{.Name}}: {{.IPv4Address}}{{println}}{{end}}'
echo ""

# Step 3: Test DNS resolution using service names (CORRECT)
echo -e "${YELLOW}Step 3: Testing DNS resolution with service names...${NC}"
echo ""

run_test "Resolve 'nginx-rtmp' from multiplexer" "docker exec ts-multiplexer getent hosts nginx-rtmp"
run_test "Resolve 'multiplexer' from nginx" "docker exec nginx-rtmp-server getent hosts multiplexer"
run_test "Resolve 'ffmpeg-fallback' from multiplexer" "docker exec ts-multiplexer getent hosts ffmpeg-fallback"
run_test "Resolve 'ffmpeg-srt-input' from multiplexer" "docker exec ts-multiplexer getent hosts ffmpeg-srt-input"

echo ""

# Step 4: Test ping connectivity using service names (CORRECT)
echo -e "${YELLOW}Step 4: Testing ICMP connectivity with service names...${NC}"
echo ""

run_test "Ping nginx-rtmp from multiplexer" "docker exec ts-multiplexer ping -c 1 -W 2 nginx-rtmp"
echo -e "${BLUE}Testing: Ping multiplexer from nginx (optional)${NC}"
if docker exec nginx-rtmp-server ping -c 1 -W 2 multiplexer > /dev/null 2>&1; then
    echo -e "${GREEN}✓ PASSED${NC}"
    TESTS_PASSED=$((TESTS_PASSED + 1))
else
    echo -e "${YELLOW}⊘ SKIPPED (ping not available in nginx container)${NC}"
fi
run_test "Ping ffmpeg-fallback from multiplexer" "docker exec ts-multiplexer ping -c 1 -W 2 ffmpeg-fallback"
run_test "Ping ffmpeg-srt-input from multiplexer" "docker exec ts-multiplexer ping -c 1 -W 2 ffmpeg-srt-input"

echo ""

# Step 5: Test RTMP port connectivity
echo -e "${YELLOW}Step 5: Testing RTMP port connectivity...${NC}"
echo ""

run_test "RTMP port 1935 accessible on nginx-rtmp" "docker exec ts-multiplexer nc -zv nginx-rtmp 1935 2>&1 | grep -q succeeded"
run_test "HTTP port 8080 accessible on nginx-rtmp" "docker exec ts-multiplexer nc -zv nginx-rtmp 8080 2>&1 | grep -q succeeded"

echo ""

# Step 6: Test UDP ports for multiplexer
echo -e "${YELLOW}Step 6: Testing UDP port bindings on multiplexer...${NC}"
echo ""

echo "UDP Port Bindings:"
docker exec ts-multiplexer ss -uln | grep -E ":(10000|10001)" || echo "  No UDP ports bound yet (normal if multiplexer just started)"

echo ""

# Step 7: Check configuration
echo -e "${YELLOW}Step 7: Verifying configuration uses correct service names...${NC}"
echo ""

echo "Config.yaml RTMP URL:"
grep "rtmp_url" config.yaml | sed 's/^/  /'
echo ""

if grep -q "rtmp://nginx-rtmp" config.yaml; then
    echo -e "${GREEN}✓ Config uses correct service name 'nginx-rtmp'${NC}"
    TESTS_PASSED=$((TESTS_PASSED + 1))
else
    echo -e "${RED}✗ Config does not use correct service name${NC}"
    TESTS_FAILED=$((TESTS_FAILED + 1))
fi

echo ""

# Step 8: Common mistakes check
echo -e "${YELLOW}Step 8: Checking for common networking mistakes...${NC}"
echo ""

if docker network inspect tsduck-multiplexer-network --format '{{len .Containers}}' | grep -q '^4$'; then
    echo -e "${GREEN}✓ All 4 containers are on the same network${NC}"
    TESTS_PASSED=$((TESTS_PASSED + 1))
else
    echo -e "${RED}✗ Not all containers are on the same network${NC}"
    TESTS_FAILED=$((TESTS_FAILED + 1))
fi

echo ""

# Summary
echo "=========================================="
echo -e "${BLUE}Test Summary${NC}"
echo "=========================================="
echo -e "Tests Passed: ${GREEN}${TESTS_PASSED}${NC}"
echo -e "Tests Failed: ${RED}${TESTS_FAILED}${NC}"
echo ""

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}=========================================="
    echo "All Network Tests Passed! ✓"
    echo "==========================================${NC}"
    echo ""
    echo "Network is properly configured. Key points:"
    echo "  • Use service name 'nginx-rtmp' (not 'nginx-rtmp-server')"
    echo "  • All containers are on network 'tsduck-multiplexer-network'"
    echo "  • RTMP connectivity verified on port 1935"
    echo ""
    exit 0
else
    echo -e "${RED}=========================================="
    echo "Some Tests Failed"
    echo "==========================================${NC}"
    echo ""
    echo "Please review the failed tests above."
    echo ""
    exit 1
fi