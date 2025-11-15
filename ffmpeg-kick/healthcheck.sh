#!/bin/bash

# Check if ffmpeg process is running
if pgrep -x "ffmpeg" > /dev/null; then
    exit 0
else
    exit 1
fi