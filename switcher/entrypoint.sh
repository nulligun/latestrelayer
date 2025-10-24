#!/usr/bin/env bash
set -euo pipefail

log() {
    echo "[$(date +'%F %T')] $*" >&2
}

# Required environment variables
: "${SOURCE_MODE:?SOURCE_MODE not set (must be 'live' or 'offline')}"
: "${RTMP_APP:?RTMP_APP not set}"
: "${RTMP_STREAM_NAME:?RTMP_STREAM_NAME not set}"

# Optional with defaults
OUT_RES="${OUT_RES:-1080}"
OUT_FPS="${OUT_FPS:-30}"
VID_BITRATE="${VID_BITRATE:-6000k}"
MAX_BITRATE="${MAX_BITRATE:-6000k}"
BUFFER_SIZE="${BUFFER_SIZE:-12M}"
AUDIO_BITRATE="${AUDIO_BITRATE:-160k}"
AUDIO_SAMPLERATE="${AUDIO_SAMPLERATE:-48000}"

# Paths
OFFLINE_MP4="/opt/offline.mp4"
LIVE_RTMP="rtmp://nginx-rtmp:1935/${RTMP_APP}/${RTMP_STREAM_NAME}"
SWITCH_OUT="rtmp://nginx-rtmp:1935/switch/out"
LOGFILE="/var/log/relayer/switcher.log"

# Encoding settings matching the original bash script
VENC="-c:v libx264 -preset veryfast -profile:v high -tune zerolatency -b:v ${VID_BITRATE} -maxrate ${MAX_BITRATE} -bufsize ${BUFFER_SIZE} -pix_fmt yuv420p -g 120 -keyint_min 120 -sc_threshold 0 -r ${OUT_FPS}"
AENC="-c:a aac -b:a ${AUDIO_BITRATE} -ar ${AUDIO_SAMPLERATE} -ac 2"

log "Starting switcher in ${SOURCE_MODE} mode..."
log "Output: ${SWITCH_OUT}"
log "Resolution: ${OUT_RES}p @ ${OUT_FPS}fps"
log "Video bitrate: ${VID_BITRATE} (max: ${MAX_BITRATE})"

if [[ "${SOURCE_MODE}" == "live" ]]; then
    log "Publishing LIVE stream from ${LIVE_RTMP}"
    
    exec ffmpeg -hide_banner -loglevel warning \
        -reconnect 1 -reconnect_streamed 1 -reconnect_on_network_error 1 \
        -rtmp_live live -i "${LIVE_RTMP}" \
        -filter_complex "[0:v]scale=1280:720:flags=bicubic,fps=${OUT_FPS}[v];[0:a]aresample=${AUDIO_SAMPLERATE},adelay=0|0[a]" \
        -map "[v]" -map "[a]" ${VENC} ${AENC} \
        -f flv "${SWITCH_OUT}" \
        >>"${LOGFILE}" 2>&1

elif [[ "${SOURCE_MODE}" == "offline" ]]; then
    # Validate offline file exists
    if [[ ! -f "${OFFLINE_MP4}" ]]; then
        log "ERROR: Offline file not found: ${OFFLINE_MP4}"
        exit 1
    fi
    
    log "Publishing OFFLINE loop from ${OFFLINE_MP4}"
    
    exec ffmpeg -hide_banner -loglevel warning \
        -stream_loop -1 -re -i "${OFFLINE_MP4}" \
        -filter_complex "[0:v]scale=1280:720:flags=bicubic,fps=${OUT_FPS}[v];[0:a]aresample=${AUDIO_SAMPLERATE}[a]" \
        -map "[v]" -map "[a]" ${VENC} ${AENC} \
        -f flv "${SWITCH_OUT}" \
        >>"${LOGFILE}" 2>&1

else
    log "ERROR: Invalid SOURCE_MODE '${SOURCE_MODE}' (must be 'live' or 'offline')"
    exit 1
fi