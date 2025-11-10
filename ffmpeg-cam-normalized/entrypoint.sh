#!/bin/bash
set -e

echo "=========================================="
echo "FFmpeg Camera Relay - Starting"
echo "=========================================="
echo "Input:  rtmp://nginx-rtmp:1936/live/cam-raw"
echo "Output: rtmp://nginx-rtmp:1936/live/cam"
echo "Purpose: Normalize camera streams for GStreamer compatibility"
echo ""

# Configuration with defaults
VIDEO_BITRATE=${VIDEO_BITRATE:-3000}
AUDIO_BITRATE=${AUDIO_BITRATE:-128}
FRAMERATE=${FRAMERATE:-30}
GOP_SIZE=${GOP_SIZE:-60}
MAX_RETRIES=${MAX_RETRIES:-0}  # 0 = infinite retries
RETRY_DELAY=${RETRY_DELAY:-5}
STREAM_CHECK_INTERVAL=${STREAM_CHECK_INTERVAL:-5}
NGINX_STATS_URL="http://nginx-rtmp:8080/stat"

echo "Configuration:"
echo "  Video Bitrate: ${VIDEO_BITRATE}k"
echo "  Audio Bitrate: ${AUDIO_BITRATE}k"
echo "  Frame Rate: ${FRAMERATE} fps"
echo "  GOP Size: ${GOP_SIZE} frames (2s keyframes)"
echo "  Retry Delay: ${RETRY_DELAY}s"
echo "  Stream Check Interval: ${STREAM_CHECK_INTERVAL}s"
echo "=========================================="
echo ""

# Function to check if cam-raw stream exists
check_stream_exists() {
    # Query nginx-rtmp stats and check for cam-raw stream
    if curl -s --max-time 2 "$NGINX_STATS_URL" | grep -q "<name>cam-raw</name>"; then
        return 0  # Stream exists
    else
        return 1  # Stream doesn't exist
    fi
}

# Wait for nginx-rtmp to be ready
echo "[init] Waiting for nginx-rtmp to be ready..."
sleep 5

# Wait for cam-raw stream to become available
echo "[init] Waiting for cam-raw stream to be published..."
while true; do
    if check_stream_exists; then
        echo "[init] ✓ Stream detected on /live/cam-raw"
        break
    else
        echo "[init] No stream found on /live/cam-raw, waiting ${STREAM_CHECK_INTERVAL}s..."
        sleep "$STREAM_CHECK_INTERVAL"
    fi
done

retry_count=0

while true; do
    if [ "$MAX_RETRIES" -ne 0 ] && [ "$retry_count" -ge "$MAX_RETRIES" ]; then
        echo "[error] Maximum retries ($MAX_RETRIES) reached. Exiting."
        exit 1
    fi
    
    if [ "$retry_count" -gt 0 ]; then
        echo ""
        echo "[retry] Attempt #$retry_count - Waiting ${RETRY_DELAY}s before reconnecting..."
        sleep "$RETRY_DELAY"
        
        # Wait for stream to come back
        echo "[retry] Checking if cam-raw stream is available..."
        while true; do
            if check_stream_exists; then
                echo "[retry] ✓ Stream detected, reconnecting..."
                break
            else
                echo "[retry] Stream not available yet, waiting ${STREAM_CHECK_INTERVAL}s..."
                sleep "$STREAM_CHECK_INTERVAL"
            fi
        done
    fi
    
    retry_count=$((retry_count + 1))
    
    echo "[stream] Starting FFmpeg relay (attempt #$retry_count)..."
    echo "[stream] Reading from: rtmp://nginx-rtmp:1936/live/cam-raw"
    echo "[stream] Publishing to: rtmp://nginx-rtmp:1936/live/cam"
    
    # Run FFmpeg with live stream transcoding
    # NOTE: No -re flag for live RTMP inputs (only for files)
    # -i: Input from nginx-rtmp cam-raw stream
    # -c:v libx264: Encode video with H.264
    # -profile:v main: Use Main profile (GStreamer compatible, better than Baseline)
    # -level 4.0: H.264 level for 1080p
    # -pix_fmt yuv420p: Pixel format (most compatible)
    # -preset veryfast: Encoding speed vs compression tradeoff
    # -tune zerolatency: Optimize for low latency
    # -bf 0: No B-frames (critical for GStreamer compatibility)
    # -b:v: Video bitrate
    # -maxrate/bufsize: Rate control for stable bitrate
    # -r: Output framerate
    # -g: GOP size (keyframe interval)
    # -keyint_min: Minimum keyframe interval
    # -c:a aac: Encode audio with AAC
    # -b:a: Audio bitrate
    # -ar: Audio sample rate
    # -ac: Audio channels (stereo)
    # -f flv: Output format (FLV for RTMP)
    
    ffmpeg -nostdin -loglevel info pipe:1 -nostats \
        -i rtmp://nginx-rtmp:1936/live/cam-raw \
        -c:v libx264 \
        -profile:v main \
        -level 4.0 \
        -pix_fmt yuv420p \
        -preset veryfast \
        -tune zerolatency \
        -bf 0 \
        -b:v ${VIDEO_BITRATE}k \
        -maxrate ${VIDEO_BITRATE}k \
        -bufsize $((VIDEO_BITRATE * 2))k \
        -r ${FRAMERATE} \
        -g ${GOP_SIZE} \
        -keyint_min ${GOP_SIZE} \
        -c:a aac \
        -b:a ${AUDIO_BITRATE}k \
        -ar 48000 \
        -ac 2 \
        -f flv \
        rtmp://nginx-rtmp:1936/live/cam
    
    exit_code=$?
    
    if [ $exit_code -eq 0 ]; then
        echo "[stream] FFmpeg exited normally"
    else
        echo "[error] FFmpeg exited with code $exit_code"
    fi
    
    echo "[stream] Stream ended or disconnected. Will check for stream and retry..."
done