#!/usr/bin/env python3
"""
RTMP Relay Supervisor
Monitors nginx-rtmp stats and manages switcher FFmpeg process lifecycle
"""

import os
import sys
import time
import logging
import requests
import subprocess
import signal
from lxml import etree
from datetime import datetime

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='[%(asctime)s] %(levelname)s: %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

# Environment configuration
RTMP_APP = os.getenv('RTMP_APP', 'live')
RTMP_STREAM_NAME = os.getenv('RTMP_STREAM_NAME', 'mystream')
POLL_INTERVAL = int(os.getenv('POLL_INTERVAL', '1'))
CRASH_BACKOFF = int(os.getenv('CRASH_BACKOFF', '2'))
OUT_RES = os.getenv('OUT_RES', '1080')
OUT_FPS = os.getenv('OUT_FPS', '30')
VID_BITRATE = os.getenv('VID_BITRATE', '6000k')
MAX_BITRATE = os.getenv('MAX_BITRATE', '6000k')
BUFFER_SIZE = os.getenv('BUFFER_SIZE', '12M')
AUDIO_BITRATE = os.getenv('AUDIO_BITRATE', '160k')
AUDIO_SAMPLERATE = os.getenv('AUDIO_SAMPLERATE', '48000')

# Paths
OFFLINE_MP4 = "/opt/offline.mp4"
LIVE_RTMP = f"rtmp://nginx-rtmp:1935/{RTMP_APP}/{RTMP_STREAM_NAME}"
SWITCH_OUT = "rtmp://nginx-rtmp:1935/switch/out"

# Service endpoints
NGINX_STATS_URL = 'http://nginx-rtmp:8080/rtmp_stat'

# State tracking
current_mode = None
switcher_process = None
last_check_time = None
error_count = 0
MAX_ERRORS = 10


def log_mode_change(from_mode, to_mode):
    """Log mode transitions"""
    logger.info(f"Mode change: {from_mode} -> {to_mode}")


def is_private_ip(ip):
    """Check if IP is loopback or RFC1918 private"""
    if not ip:
        return True
    
    # Loopback
    if ip.startswith('127.'):
        return True
    
    # RFC1918 private ranges
    if ip.startswith('10.'):
        return True
    if ip.startswith('192.168.'):
        return True
    
    # 172.16.0.0 - 172.31.255.255
    parts = ip.split('.')
    if len(parts) >= 2 and parts[0] == '172':
        second_octet = int(parts[1])
        if 16 <= second_octet <= 31:
            return True
    
    return False


def check_live_stream():
    """
    Check if live stream is active and being published from external source
    Returns: (is_live: bool, details: str)
    """
    try:
        response = requests.get(NGINX_STATS_URL, timeout=5)
        response.raise_for_status()
        
        # Parse XML stats
        root = etree.fromstring(response.content)
        
        # Find our specific stream
        xpath = f"//application[name='{RTMP_APP}']/live/stream[name='{RTMP_STREAM_NAME}']"
        streams = root.xpath(xpath)
        
        if not streams:
            return False, "Stream not found in stats"
        
        stream = streams[0]
        
        # Check video bitrate
        bw_video_elem = stream.find('bw_video')
        if bw_video_elem is None or not bw_video_elem.text:
            return False, "No video bitrate data"
        
        bw_video = int(bw_video_elem.text)
        if bw_video <= 0:
            return False, f"Video bitrate is {bw_video}"
        
        # Check for active publishers
        clients = stream.findall('client')
        publishing_clients = [c for c in clients if c.find('publishing') is not None]
        
        if not publishing_clients:
            return False, "No publishing clients"
        
        # Check if any publisher is from external (non-private) IP
        external_publishers = []
        for client in publishing_clients:
            address_elem = client.find('address')
            if address_elem is not None and address_elem.text:
                ip = address_elem.text
                if not is_private_ip(ip):
                    external_publishers.append(ip)
        
        if not external_publishers:
            return False, f"Only private/loopback publishers found"
        
        # Stream is live!
        return True, f"External publisher(s): {', '.join(external_publishers)}, video bitrate: {bw_video}"
        
    except requests.exceptions.RequestException as e:
        return False, f"HTTP error: {e}"
    except etree.XMLSyntaxError as e:
        return False, f"XML parse error: {e}"
    except Exception as e:
        return False, f"Unexpected error: {e}"


def stop_switcher():
    """Stop the current switcher process"""
    global switcher_process
    
    if switcher_process is None:
        return
    
    try:
        logger.info("Stopping switcher process...")
        switcher_process.terminate()
        
        # Wait up to 3 seconds for graceful shutdown
        try:
            switcher_process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            logger.warning("Switcher did not stop gracefully, killing...")
            switcher_process.kill()
            switcher_process.wait()
        
        logger.info("Switcher process stopped")
        switcher_process = None
        
    except Exception as e:
        logger.error(f"Error stopping switcher: {e}")
        switcher_process = None


def start_switcher_live():
    """Start switcher publishing live stream"""
    global switcher_process
    
    logger.info("Starting switcher in LIVE mode")
    
    # Build FFmpeg command for live mode
    venc = f"-c:v libx264 -preset veryfast -profile:v high -tune zerolatency -b:v {VID_BITRATE} -maxrate {MAX_BITRATE} -bufsize {BUFFER_SIZE} -pix_fmt yuv420p -g 120 -keyint_min 120 -sc_threshold 0 -r {OUT_FPS}"
    aenc = f"-c:a aac -b:a {AUDIO_BITRATE} -ar {AUDIO_SAMPLERATE} -ac 2"
    
    cmd = [
        'ffmpeg', '-hide_banner', '-loglevel', 'warning',
        '-reconnect', '1', '-reconnect_streamed', '1', '-reconnect_on_network_error', '1',
        '-rtmp_live', 'live', '-i', LIVE_RTMP,
        '-filter_complex', f'[0:v]scale=1280:720:flags=bicubic,fps={OUT_FPS}[v];[0:a]aresample={AUDIO_SAMPLERATE},adelay=0|0[a]',
        '-map', '[v]', '-map', '[a]'
    ]
    cmd.extend(venc.split())
    cmd.extend(aenc.split())
    cmd.extend(['-f', 'flv', SWITCH_OUT])
    
    try:
        switcher_process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            preexec_fn=os.setsid  # Create new process group
        )
        logger.info(f"Switcher LIVE process started (PID: {switcher_process.pid})")
        return True
    except Exception as e:
        logger.error(f"Failed to start switcher LIVE: {e}")
        return False


def start_switcher_offline():
    """Start switcher publishing offline video loop"""
    global switcher_process
    
    logger.info("Starting switcher in OFFLINE mode")
    
    # Check offline file exists
    if not os.path.exists(OFFLINE_MP4):
        logger.error(f"Offline file not found: {OFFLINE_MP4}")
        return False
    
    # Build FFmpeg command for offline mode
    venc = f"-c:v libx264 -preset veryfast -profile:v high -tune zerolatency -b:v {VID_BITRATE} -maxrate {MAX_BITRATE} -bufsize {BUFFER_SIZE} -pix_fmt yuv420p -g 120 -keyint_min 120 -sc_threshold 0 -r {OUT_FPS}"
    aenc = f"-c:a aac -b:a {AUDIO_BITRATE} -ar {AUDIO_SAMPLERATE} -ac 2"
    
    cmd = [
        'ffmpeg', '-hide_banner', '-loglevel', 'warning',
        '-stream_loop', '-1', '-re', '-i', OFFLINE_MP4,
        '-filter_complex', f'[0:v]scale=1280:720:flags=bicubic,fps={OUT_FPS}[v];[0:a]aresample={AUDIO_SAMPLERATE}[a]',
        '-map', '[v]', '-map', '[a]'
    ]
    cmd.extend(venc.split())
    cmd.extend(aenc.split())
    cmd.extend(['-f', 'flv', SWITCH_OUT])
    
    try:
        switcher_process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            preexec_fn=os.setsid  # Create new process group
        )
        logger.info(f"Switcher OFFLINE process started (PID: {switcher_process.pid})")
        return True
    except Exception as e:
        logger.error(f"Failed to start switcher OFFLINE: {e}")
        return False


def switch_to_live():
    """Switch to live stream by restarting switcher process"""
    stop_switcher()
    time.sleep(0.5)  # Brief pause like the bash script
    return start_switcher_live()


def switch_to_offline():
    """Switch to offline video by restarting switcher process"""
    stop_switcher()
    time.sleep(0.5)  # Brief pause like the bash script
    return start_switcher_offline()


def cleanup_handler(signum, frame):
    """Handle shutdown signals"""
    logger.info(f"Received signal {signum}, cleaning up...")
    stop_switcher()
    sys.exit(0)


def main():
    """Main supervisor loop"""
    global current_mode, last_check_time, error_count
    
    # Register signal handlers
    signal.signal(signal.SIGTERM, cleanup_handler)
    signal.signal(signal.SIGINT, cleanup_handler)
    
    logger.info("="*60)
    logger.info("RTMP Relay Supervisor Starting")
    logger.info("="*60)
    logger.info(f"Stream: {RTMP_APP}/{RTMP_STREAM_NAME}")
    logger.info(f"Poll interval: {POLL_INTERVAL}s")
    logger.info(f"nginx-rtmp stats: {NGINX_STATS_URL}")
    logger.info(f"Switcher output: {SWITCH_OUT}")
    logger.info("="*60)
    
    # Wait for services to be ready
    logger.info("Waiting for services to be ready...")
    time.sleep(5)
    
    # Initial check and mode setup
    is_live, details = check_live_stream()
    if is_live:
        logger.info(f"Initial check: Stream is LIVE - {details}")
        if switch_to_live():
            current_mode = 'live'
    else:
        logger.info(f"Initial check: Stream is OFFLINE - {details}")
        if switch_to_offline():
            current_mode = 'offline'
    
    # Main monitoring loop
    while True:
        try:
            time.sleep(POLL_INTERVAL)
            last_check_time = datetime.now()
            
            # Check if switcher process is still running
            if switcher_process and switcher_process.poll() is not None:
                logger.warning(f"Switcher process died unexpectedly! Restarting in {current_mode} mode...")
                if current_mode == 'live':
                    start_switcher_live()
                else:
                    start_switcher_offline()
            
            is_live, details = check_live_stream()
            
            # Log check result (verbose)
            logger.debug(f"Check: mode={current_mode}, live={is_live}, {details}")
            
            # Determine if we need to switch modes
            if is_live and current_mode != 'live':
                logger.info(f"Stream became LIVE: {details}")
                if switch_to_live():
                    log_mode_change(current_mode, 'live')
                    current_mode = 'live'
                    error_count = 0
                    
            elif not is_live and current_mode != 'offline':
                logger.info(f"Stream went OFFLINE: {details}")
                if switch_to_offline():
                    log_mode_change(current_mode, 'offline')
                    current_mode = 'offline'
                    error_count = 0
            
            # Log status every 60 checks (roughly every minute at 1s poll)
            if int(time.time()) % 60 < POLL_INTERVAL:
                logger.info(f"Status: mode={current_mode}, live_detected={is_live}")
            
        except KeyboardInterrupt:
            logger.info("Received shutdown signal")
            break
            
        except Exception as e:
            error_count += 1
            logger.error(f"Error in main loop (count: {error_count}): {e}", exc_info=True)
            
            if error_count >= MAX_ERRORS:
                logger.critical(f"Too many errors ({error_count}), backing off for {CRASH_BACKOFF}s")
                time.sleep(CRASH_BACKOFF)
                error_count = 0
    
    stop_switcher()
    logger.info("Supervisor shutting down")


if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        logger.critical(f"Fatal error: {e}", exc_info=True)
        stop_switcher()
        sys.exit(1)