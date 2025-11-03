#!/usr/bin/env python3
"""
RTMP Auto-Switcher Service

Monitors nginx-rtmp stats and automatically switches between camera and brb streams
based on stream availability and bitrate quality.
"""
import os
import sys
import time
import json
import xml.etree.ElementTree as ET
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError
from urllib.parse import urlencode

# Force unbuffered output
sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

print("=" * 60, flush=True)
print("RTMP Auto-Switcher - Starting Up", flush=True)
print("=" * 60, flush=True)

# --- CONFIG FROM ENVIRONMENT ---
STAT_URL = os.getenv("STAT_URL", "http://nginx-rtmp:8080/stat")
MUXER_HEALTH_URL = os.getenv("MUXER_HEALTH_URL", "http://muxer:8088/health")
APP_NAME = os.getenv("APP_NAME", "live")
CAM_STREAM = os.getenv("CAM_STREAM", "cam")
BRB_NAME = os.getenv("BRB_NAME", "brb")
SWITCH_API = os.getenv("SWITCH_API", "http://muxer:8088/switch")

# Bitrate threshold in kilobits per second
MIN_BITRATE_KBPS = int(os.getenv("MIN_BITRATE_KBPS", "300"))

# Watchdog tuning
POLL_SECS = float(os.getenv("POLL_SECS", "0.5"))
CAM_MISS_TIMEOUT = float(os.getenv("CAM_MISS_TIMEOUT", "3.0"))
CAM_BACK_STABILITY = float(os.getenv("CAM_BACK_STABILITY", "2.0"))

print(f"[config] STAT_URL: {STAT_URL}", flush=True)
print(f"[config] MUXER_HEALTH_URL: {MUXER_HEALTH_URL}", flush=True)
print(f"[config] APP_NAME: {APP_NAME}", flush=True)
print(f"[config] CAM_STREAM: {CAM_STREAM}", flush=True)
print(f"[config] BRB_NAME: {BRB_NAME}", flush=True)
print(f"[config] SWITCH_API: {SWITCH_API}", flush=True)
print(f"[config] MIN_BITRATE_KBPS: {MIN_BITRATE_KBPS}", flush=True)
print(f"[config] POLL_SECS: {POLL_SECS}", flush=True)
print(f"[config] CAM_MISS_TIMEOUT: {CAM_MISS_TIMEOUT}", flush=True)
print(f"[config] CAM_BACK_STABILITY: {CAM_BACK_STABILITY}", flush=True)
print("=" * 60, flush=True)

# --- INTERNAL STATE ---
last_seen_cam = time.monotonic()  # last time cam was seen alive with good bitrate
active_src = None                  # "cam" or "brb"
cam_stable_since = None            # timestamp when cam became stable


def http_get(url, timeout=2.0):
    """Fetch content from URL with timeout."""
    req = Request(url, headers={"User-Agent": "rtmp-auto-switcher/1.0"})
    with urlopen(req, timeout=timeout) as r:
        return r.read()


def call_switch(src):
    """Call the stream switcher API to change source."""
    global active_src
    try:
        q = urlencode({"src": src})
        urlopen(f"{SWITCH_API}?{q}", timeout=2.0).read()
        print(f"[switch] -> {src}", flush=True)
        active_src = src
        return True
    except Exception as e:
        print(f"[warn] switch API failed: {e}", file=sys.stderr, flush=True)
        return False


def check_muxer_health():
    """
    Query muxer health endpoint to check if cam is using fallback source.
    
    Returns dict with:
    - using_fallback: bool - whether cam is using test pattern fallback
    - stream_exists: bool - whether real cam stream exists on nginx
    """
    try:
        req = Request(MUXER_HEALTH_URL, headers={"User-Agent": "rtmp-auto-switcher/1.0"})
        with urlopen(req, timeout=1.5) as r:
            json_data = r.read()
        
        health = json.loads(json_data)
        cam_status = health.get("sources", {}).get("cam", {})
        
        return {
            "using_fallback": cam_status.get("using_fallback", False),
            "stream_exists": cam_status.get("stream_exists", False)
        }
    except (URLError, HTTPError, TimeoutError, json.JSONDecodeError, Exception) as e:
        print(f"[warn] muxer health check failed: {e}", file=sys.stderr, flush=True)
        # On error, assume no fallback but also no real stream
        return {"using_fallback": False, "stream_exists": False}


def parse_stream_stats(xml_bytes):
    """
    Parse nginx-rtmp stats XML and extract camera stream information.
    
    Returns dict with:
    - exists: bool - whether the stream exists
    - publishing: bool - whether there's an active publisher
    - bw_video: int - video bandwidth in bytes/sec (0 if not available)
    - nclients: int - number of connected clients
    """
    try:
        root = ET.fromstring(xml_bytes)
    except ET.ParseError as e:
        print(f"[warn] XML parse error: {e}", file=sys.stderr, flush=True)
        return {"exists": False, "publishing": False, "bw_video": 0, "nclients": 0}

    # Navigate: rtmp -> server -> application -> stream
    for server in root.findall(".//server"):
        for app in server.findall(".//application"):
            app_name = app.findtext("name", "")
            if app_name != APP_NAME:
                continue
            
            # Found the correct application, now look for the camera stream
            for stream in app.findall(".//stream"):
                stream_name = stream.findtext("name", "")
                if stream_name != CAM_STREAM:
                    continue
                
                # Found the camera stream, extract metrics
                publishing_text = stream.findtext("publishing", "")
                bw_video_text = stream.findtext("bw_video", "0")
                nclients_text = stream.findtext("nclients", "0")
                
                # Parse values
                try:
                    bw_video = int(bw_video_text)
                except ValueError:
                    bw_video = 0
                
                try:
                    nclients = int(nclients_text)
                except ValueError:
                    nclients = 0
                
                # Check if actively publishing
                # Different nginx-rtmp versions use different values
                is_publishing = (
                    publishing_text.lower() in ("active", "1", "true", "on") or
                    nclients >= 1  # If clients are connected, likely publishing
                )
                
                return {
                    "exists": True,
                    "publishing": is_publishing,
                    "bw_video": bw_video,
                    "nclients": nclients
                }
    
    # Stream not found
    return {"exists": False, "publishing": False, "bw_video": 0, "nclients": 0}


def check_bitrate_sufficient(bw_video_bytes_per_sec):
    """
    Check if video bitrate meets minimum threshold.
    
    Args:
        bw_video_bytes_per_sec: Video bandwidth in bytes per second
    
    Returns:
        bool: True if bitrate meets or exceeds threshold
    """
    if bw_video_bytes_per_sec == 0:
        return False
    
    # Convert bytes/sec to kilobits/sec: (bytes * 8) / 1000
    bitrate_kbps = (bw_video_bytes_per_sec * 8) / 1000
    
    sufficient = bitrate_kbps >= MIN_BITRATE_KBPS
    
    if not sufficient:
        print(
            f"[bitrate] INSUFFICIENT: {bitrate_kbps:.1f} kbps < {MIN_BITRATE_KBPS} kbps threshold",
            flush=True
        )
    
    return sufficient


def is_cam_alive_and_healthy(stats, muxer_health):
    """
    Determine if camera stream is alive and meets quality requirements.
    
    Args:
        stats: Dictionary returned from parse_stream_stats()
        muxer_health: Dictionary returned from check_muxer_health()
    
    Returns:
        bool: True if stream exists, is publishing, has sufficient bitrate,
              and is NOT using fallback test source
    """
    # CRITICAL: Reject if using fallback test source
    # This prevents auto-switching to cam when it's only showing test pattern
    if muxer_health.get("using_fallback", False):
        return False
    
    if not stats["exists"]:
        return False
    
    if not stats["publishing"]:
        return False
    
    return check_bitrate_sufficient(stats["bw_video"])


def main():
    """Main monitoring loop."""
    global last_seen_cam, cam_stable_since, active_src

    # Start on brb to be safe
    print("[init] Setting initial source to brb", flush=True)
    call_switch("brb")
    
    # Wait a moment for the switch to take effect
    time.sleep(1)

    print("[main] Entering monitoring loop", flush=True)
    print("=" * 60, flush=True)

    while True:
        try:
            # Fetch and parse stats from both nginx and muxer
            xml = http_get(STAT_URL, timeout=1.5)
            stats = parse_stream_stats(xml)
            muxer_health = check_muxer_health()
            cam_healthy = is_cam_alive_and_healthy(stats, muxer_health)
            
            # Log when cam is rejected due to fallback
            if stats["exists"] and stats["publishing"] and muxer_health.get("using_fallback", False):
                print(
                    f"[state] Camera stream using FALLBACK source (test pattern). "
                    f"Treated as unhealthy - will not auto-switch.",
                    flush=True
                )
            
            # Log current status periodically (every 10 seconds when nothing changes)
            now = time.monotonic()
            
        except (URLError, HTTPError, TimeoutError, Exception) as e:
            print(f"[warn] stat fetch failed: {e}", file=sys.stderr, flush=True)
            cam_healthy = False
            now = time.monotonic()

        # --- STATE MACHINE LOGIC ---
        
        if cam_healthy:
            # Camera is present and meets quality requirements
            last_seen_cam = now
            
            # If currently on brb, check if cam is stable enough to switch back
            if active_src != "cam":
                if cam_stable_since is None:
                    cam_stable_since = now
                    print(
                        f"[state] Camera detected with good quality "
                        f"(bitrate: {(stats['bw_video'] * 8) / 1000:.1f} kbps). "
                        f"Waiting {CAM_BACK_STABILITY}s for stability...",
                        flush=True
                    )
                else:
                    elapsed = now - cam_stable_since
                    if elapsed >= CAM_BACK_STABILITY:
                        print(
                            f"[state] Camera stable for {elapsed:.1f}s. Switching to cam.",
                            flush=True
                        )
                        call_switch("cam")
                        cam_stable_since = None
                    # else: still waiting for stability
        else:
            # Camera is not healthy (missing, not publishing, or insufficient bitrate)
            cam_stable_since = None
            
            # If we're on cam and it's been missing/unhealthy for too long, fall back
            if active_src == "cam":
                time_since_good = now - last_seen_cam
                if time_since_good >= CAM_MISS_TIMEOUT:
                    if stats["exists"]:
                        print(
                            f"[state] Camera quality degraded for {time_since_good:.1f}s "
                            f"(bitrate: {(stats['bw_video'] * 8) / 1000:.1f} kbps). "
                            f"Switching to brb.",
                            flush=True
                        )
                    else:
                        print(
                            f"[state] Camera missing for {time_since_good:.1f}s. "
                            f"Switching to brb.",
                            flush=True
                        )
                    call_switch("brb")

        time.sleep(POLL_SECS)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[main] Received interrupt signal. Shutting down.", flush=True)
        sys.exit(0)
    except Exception as e:
        print(f"[main] FATAL ERROR: {e}", file=sys.stderr, flush=True)
        import traceback
        traceback.print_exc()
        sys.exit(1)