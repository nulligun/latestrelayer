#!/usr/bin/env python3
"""
RTMP Relay Supervisor
Monitors nginx-rtmp stats and controls FFmpeg stream switching via ZMQ
"""

import os
import sys
import time
import logging
import requests
import zmq
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

# Service endpoints
NGINX_STATS_URL = 'http://nginx-rtmp:8080/rtmp_stat'
FFMPEG_ZMQ_ENDPOINT = 'tcp://ffmpeg-relay:5559'

# State tracking
current_mode = None
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


def send_zmq_command(command):
    """
    Send ZMQ command to FFmpeg
    Commands: 
      - "streamselect map 0" - switch video to live (input 0)
      - "streamselect map 1" - switch video to offline (input 1)
      - "aselect map 0" - switch audio to live
      - "aselect map 1" - switch audio to offline
    """
    try:
        context = zmq.Context()
        socket = context.socket(zmq.REQ)
        socket.setsockopt(zmq.LINGER, 0)
        socket.connect(FFMPEG_ZMQ_ENDPOINT)
        
        # Send command with timeout
        socket.send_string(command)
        
        # Wait for response with timeout
        if socket.poll(timeout=2000):  # 2 second timeout
            response = socket.recv_string()
            logger.debug(f"ZMQ command '{command}' response: {response}")
        else:
            logger.warning(f"ZMQ command '{command}' timed out")
        
        socket.close()
        context.term()
        return True
        
    except Exception as e:
        logger.error(f"Failed to send ZMQ command '{command}': {e}")
        return False


def switch_to_live():
    """Switch FFmpeg to live stream input"""
    logger.info("Switching to LIVE stream")
    success = True
    success &= send_zmq_command("streamselect map 0")
    success &= send_zmq_command("aselect map 0")
    return success


def switch_to_offline():
    """Switch FFmpeg to offline video input"""
    logger.info("Switching to OFFLINE video")
    success = True
    success &= send_zmq_command("streamselect map 1")
    success &= send_zmq_command("aselect map 1")
    return success


def main():
    """Main supervisor loop"""
    global current_mode, last_check_time, error_count
    
    logger.info("="*60)
    logger.info("RTMP Relay Supervisor Starting")
    logger.info("="*60)
    logger.info(f"Stream: {RTMP_APP}/{RTMP_STREAM_NAME}")
    logger.info(f"Poll interval: {POLL_INTERVAL}s")
    logger.info(f"nginx-rtmp stats: {NGINX_STATS_URL}")
    logger.info(f"FFmpeg ZMQ: {FFMPEG_ZMQ_ENDPOINT}")
    logger.info("="*60)
    
    # Wait for services to be ready
    logger.info("Waiting for services to be ready...")
    time.sleep(5)
    
    # Initial check and mode setup
    is_live, details = check_live_stream()
    if is_live:
        logger.info(f"Initial check: Stream is LIVE - {details}")
        current_mode = 'offline'  # Set to opposite so first switch works
        switch_to_live()
        current_mode = 'live'
    else:
        logger.info(f"Initial check: Stream is OFFLINE - {details}")
        current_mode = 'live'  # Set to opposite so first switch works
        switch_to_offline()
        current_mode = 'offline'
    
    # Main monitoring loop
    while True:
        try:
            time.sleep(POLL_INTERVAL)
            last_check_time = datetime.now()
            
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
    
    logger.info("Supervisor shutting down")


if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        logger.critical(f"Fatal error: {e}", exc_info=True)
        sys.exit(1)