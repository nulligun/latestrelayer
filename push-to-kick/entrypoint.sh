#!/usr/bin/env bash
set -euo pipefail

log() {
    echo "[$(date +'%F %T')] $*" >&2
}

# Required environment variables
: "${KICK_URL:?KICK_URL not set}"
: "${KICK_KEY:?KICK_KEY not set}"

# Source stream from /switch/out
SRC="rtmp://nginx-rtmp:1935/switch/out"
INGEST="${KICK_URL}/${KICK_KEY}"
LOGFILE="/var/log/relayer/push-to-kick.log"

log "========================================"
log "Starting stable push to Kick.com"
log "========================================"
log "Source: ${SRC}"
log "Destination: ${KICK_URL}/***"
log "This process will maintain a continuous connection"
log "========================================"

# Wait a moment for switcher to start publishing to /switch/out
log "Waiting 3 seconds for switcher to initialize..."
sleep 3

# Stable relay with copy codec (no re-encoding)
# This maintains persistent connection to Kick regardless of switcher restarts
log "Launching stable relay to Kick..."

exec ffmpeg -hide_banner -loglevel warning \
    -reconnect 1 -reconnect_streamed 1 -reconnect_on_network_error 1 \
    -rw_timeout 5000000 \
    -i "${SRC}" \
    -c copy \
    -f flv "${INGEST}" \
    >>"${LOGFILE}" 2>&1