#!/bin/bash

set -e

echo "=== TS Loop to RTMP - Test Script ==="
echo ""

# Check if RTMP server is running
check_rtmp_server() {
    echo "Checking for RTMP server on port 1935..."
    if nc -z localhost 1935 2>/dev/null; then
        echo "✓ RTMP server is running"
        return 0
    else
        echo "✗ RTMP server not detected on port 1935"
        echo ""
        echo "Would you like to start a test RTMP server? (y/n)"
        read -r response
        if [[ "$response" =~ ^[Yy]$ ]]; then
            echo "Starting nginx-rtmp server..."
            docker run -d -p 1935:1935 --name test-rtmp-server tiangolo/nginx-rtmp
            sleep 2
            echo "✓ RTMP server started"
            return 0
        else
            echo "Please start an RTMP server before testing"
            return 1
        fi
    fi
}

# Check if test file exists
check_test_file() {
    echo ""
    echo "Checking for test TS file..."
    if [ -f "videos/fallback.ts" ]; then
        echo "✓ Test file exists: videos/fallback.ts"
        return 0
    else
        echo "✗ Test file not found: videos/fallback.ts"
        echo ""
        echo "Would you like to generate a test file? (y/n)"
        read -r response
        if [[ "$response" =~ ^[Yy]$ ]]; then
            echo "Generating test content..."
            ./generate_fallback.sh -d 10 -p smptebars
            ./convert_fallback.sh
            echo "✓ Test file generated"
            return 0
        else
            echo "Please generate a test file before testing"
            return 1
        fi
    fi
}

# Build Docker image
build_image() {
    echo ""
    echo "Building Docker image..."
    docker-compose build
    echo "✓ Docker image built"
}

# Start streaming
start_streaming() {
    echo ""
    echo "Starting TS loop streamer..."
    echo "This will stream to: rtmp://localhost/live/stream"
    echo ""
    echo "Press Ctrl+C to stop"
    echo ""
    
    docker-compose up
}

# Test with VLC
test_with_vlc() {
    echo ""
    echo "Testing stream with VLC..."
    
    if command -v vlc &> /dev/null; then
        vlc rtmp://localhost/live/stream &
        echo "✓ VLC launched"
    else
        echo "✗ VLC not found. Please install VLC or open rtmp://localhost/live/stream manually"
    fi
}

# Main flow
main() {
    # Check prerequisites
    if ! check_rtmp_server; then
        exit 1
    fi
    
    if ! check_test_file; then
        exit 1
    fi
    
    # Build
    build_image
    
    # Ask if user wants to test with VLC
    echo ""
    echo "Setup complete! Ready to start streaming."
    echo ""
    echo "Options:"
    echo "  1) Start streaming (you can view with VLC/OBS/FFplay)"
    echo "  2) Start streaming and launch VLC"
    echo "  3) Exit"
    echo ""
    echo -n "Choose option (1-3): "
    read -r option
    
    case $option in
        1)
            start_streaming
            ;;
        2)
            # Start streamer in background
            docker-compose up -d
            sleep 3
            test_with_vlc
            echo ""
            echo "Stream is running in background. To stop:"
            echo "  docker-compose down"
            ;;
        3)
            echo "Exiting"
            exit 0
            ;;
        *)
            echo "Invalid option"
            exit 1
            ;;
    esac
}

# Run main
main