#!/usr/bin/env bash
set -euo pipefail

log() {
    echo "[$(date +'%F %T')] $*" >&2
}

# Required environment variables
: "${KICK_URL:?KICK_URL not set}"
: "${KICK_KEY:?KICK_KEY not set}"
: "${RTMP_APP:?RTMP_APP not set}"
: "${RTMP_STREAM_NAME:?RTMP_STREAM_NAME not set}"

# Optional with defaults
OUT_RES="${OUT_RES:-1080}"
OUT_FPS="${OUT_FPS:-30}"
VID_BITRATE="${VID_BITRATE:-3000k}"
MAX_BITRATE="${MAX_BITRATE:-3500k}"
BUFFER_SIZE="${BUFFER_SIZE:-12000k}"
AUDIO_BITRATE="${AUDIO_BITRATE:-128k}"
AUDIO_SAMPLERATE="${AUDIO_SAMPLERATE:-48000}"

# Paths
OFFLINE_MP4="/opt/offline.mp4"
LIVE_RTMP="rtmp://nginx-rtmp:1935/${RTMP_APP}/${RTMP_STREAM_NAME}"
INGEST="${KICK_URL}/${KICK_KEY}"
LOGFILE="/var/log/relayer/ffmpeg.log"

# Validate offline file exists
if [[ ! -f "$OFFLINE_MP4" ]]; then
    log "ERROR: Offline file not found: $OFFLINE_MP4"
    exit 1
fi

log "Starting FFmpeg relay with persistent connection..."
log "Live input: $LIVE_RTMP"
log "Offline input: $OFFLINE_MP4"
log "Output: ${KICK_URL}/***"
log "Resolution: ${OUT_RES}p @ ${OUT_FPS}fps"

# Check if live stream has audio (with timeout)
has_audio() {
    local input="$1"
    timeout 3 ffprobe -v error -rw_timeout 1500000 \
        -select_streams a:0 -show_entries stream=codec_type \
        -of csv=p=0 "$input" 2>/dev/null | grep -q '^audio$'
}

# Build audio setup - inject silent audio if live stream lacks it
AUDIO_INPUT=""
LIVE_AUDIO_MAP="[0:a]"

log "Checking for audio in live stream (this may take a few seconds)..."
if ! has_audio "$LIVE_RTMP"; then
    log "No audio detected in live stream - will inject silent audio"
    AUDIO_INPUT="-f lavfi -i anullsrc=channel_layout=stereo:sample_rate=${AUDIO_SAMPLERATE}"
    LIVE_AUDIO_MAP="[2:a]"
else
    log "Audio detected in live stream"
fi

# Build FFmpeg command with persistent connection and ZMQ control
# Input 0: Live RTMP stream
# Input 1: Offline MP4 loop
# Input 2: Silent audio (if needed)
# ZMQ socket on tcp://0.0.0.0:5559 for control commands
# The zmq filter listens for commands to control streamselect/aselect dynamically

log "Launching FFmpeg with ZMQ control socket on tcp://0.0.0.0:5559"
log "Starting in OFFLINE mode (will switch to live when supervisor signals)"

exec ffmpeg -hide_banner -loglevel warning \
    -reconnect 1 -reconnect_streamed 1 -reconnect_on_network_error 1 \
    -thread_queue_size 4096 -rw_timeout 1500000 \
    -i "$LIVE_RTMP" \
    -stream_loop -1 -re -i "$OFFLINE_MP4" \
    ${AUDIO_INPUT} \
    -filter_complex "\
zmq=bind_address=tcp\\://0.0.0.0\\:5559; \
[0:v]fifo,format=yuv420p,scale=-2:${OUT_RES},fps=${OUT_FPS}[live_v]; \
[1:v]fifo,format=yuv420p,scale=-2:${OUT_RES},fps=${OUT_FPS}[offline_v]; \
${LIVE_AUDIO_MAP}afifo,aresample=async=1:first_pts=0[live_a]; \
[1:a]afifo,aresample=async=1:first_pts=0[offline_a]; \
[live_v][offline_v]streamselect=inputs=2:map=1[v]; \
[live_a][offline_a]aselect=inputs=2:map=1[a]" \
    -map "[v]" -map "[a]" \
    -c:v libx264 -preset veryfast -profile:v high \
    -g $((OUT_FPS*2)) -keyint_min $((OUT_FPS*2)) -sc_threshold 0 \
    -x264-params "bframes=0:ref=2:force-cfr=1" \
    -b:v "$VID_BITRATE" -maxrate "$MAX_BITRATE" -bufsize "$BUFFER_SIZE" \
    -c:a aac -b:a "$AUDIO_BITRATE" -ar "$AUDIO_SAMPLERATE" -ac 2 \
    -flvflags no_duration_filesize -rtmp_live live -rtmp_buffer 5000 \
    -f flv "$INGEST" \
    >>"$LOGFILE" 2>&1