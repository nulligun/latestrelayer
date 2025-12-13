#!/bin/bash
# Health check script for ffmpeg-srt-input container
# Verifies that FFmpeg is ready to receive SRT input or is actively streaming

set -e

# FFmpeg workflow: SRT input (UDP 1937) -> TCP output (10000)
# The TCP port won't open until FFmpeg receives SRT data
# So we check: Is SRT listener ready OR is TCP output active?

# Check if UDP port 1937 is listening (SRT input ready)
if ss -uln | grep -q ':1937 '; then
    echo "Health check passed: SRT listener ready on port 1937"
    exit 0
fi

# Check if TCP port 10000 is active (streaming to multiplexer)
if ss -tan | grep -q ':10000 '; then
    echo "Health check passed: TCP port 10000 active (streaming)"
    exit 0
fi

echo "Health check failed: No SRT listener or TCP activity"
exit 1