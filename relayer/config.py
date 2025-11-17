#!/usr/bin/env python3
"""
Configuration and Constants for Compositor
Centralizes all environment variables and constant values.
"""
import os

# Version
__version__ = "3.0.0"

# Environment Variables - Networking
VIDEO_TCP_PORT = int(os.getenv("VIDEO_TCP_PORT", "1940"))
SRT_PORT = 1937
OUTPUT_TCP_PORT = 5000
HTTP_API_PORT = int(os.getenv("HTTP_API_PORT", "8088"))

# Environment Variables - Timing
BUFFER_DELAY_MS = int(os.getenv("BUFFER_DELAY_MS", "100"))
SRT_GRACE_PERIOD_MS = int(os.getenv("SRT_GRACE_PERIOD_MS", "1500"))
VIDEO_WATCHDOG_TIMEOUT = float(os.getenv("VIDEO_WATCHDOG_TIMEOUT", "2.0"))
WATCHDOG_INTERVAL_MS = int(os.getenv("WATCHDOG_INTERVAL_MS", "500"))
ELEMENT_REMOVAL_TIMEOUT_SEC = 2

# Environment Variables - Video Encoding
X264_PRESET = os.getenv("X264_PRESET", "ultrafast")
X264_BITRATE = int(os.getenv("X264_BITRATE", "1500"))
FALLBACK_X264_BITRATE = 500  # Low bitrate for black screen fallback (pre-encoded once at startup)

# Environment Variables - Features
FALLBACK_SOURCE = os.getenv("FALLBACK_SOURCE", "").lower()
ENABLE_CPU_PROFILING = os.getenv("ENABLE_CPU_PROFILING", "false").lower() == "true"

# File Paths
PRIVACY_STATE_FILE = "/app/privacy_state.json"

# State Constants
STATE_FALLBACK_ONLY = "FALLBACK_ONLY"
STATE_VIDEO_BUFFERING = "VIDEO_BUFFERING"
STATE_VIDEO_CONNECTED = "VIDEO_CONNECTED"
STATE_SRT_BUFFERING = "SRT_BUFFERING"
STATE_SRT_CONNECTED = "SRT_CONNECTED"

# Video Format Constants
VIDEO_WIDTH = 1920
VIDEO_HEIGHT = 1080
VIDEO_FRAMERATE = 30
AUDIO_RATE = 48000
AUDIO_CHANNELS = 2

# Queue Configuration
VIDEO_QUEUE_MAX_TIME = 3000000000  # 3 seconds
SRT_VIDEO_QUEUE_MAX_TIME = 1000000000  # 1 second
AUDIO_QUEUE_MAX_TIME = 3000000000  # 3 seconds
SRT_AUDIO_QUEUE_MAX_TIME = 2000000000  # 2 seconds
COMP_QUEUE_MAX_TIME = 500000000  # 500ms

# GStreamer Constants
SRT_LATENCY_MS = 2000
AAC_BITRATE = 128000