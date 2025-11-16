#!/bin/bash
# Healthcheck script for relayer container
# Checks if the main Python process (main.py) is running

if pgrep -f "python3.*main.py" > /dev/null; then
    exit 0
else
    exit 1
fi