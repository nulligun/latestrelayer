#!/usr/bin/env python3
import sys, threading, time, socket, json
from urllib.parse import urlparse, parse_qs
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, HTTPServer
from datetime import datetime

# Force unbuffered output
sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

print("=" * 60, flush=True)
print("RTMP Stream Switcher - Starting Up", flush=True)
print("=" * 60, flush=True)

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GObject", "2.0")
from gi.repository import Gst, GObject, GLib

print("[startup] Initializing GStreamer...", flush=True)
Gst.init(None)
print("[startup] ✓ GStreamer initialized", flush=True)

# ----- CONFIG -----
NGINX_RTMP_HOST = "nginx-rtmp"
NGINX_RTMP_PORT = 1936
NGINX_STATS_URL = f"http://{NGINX_RTMP_HOST}:8080/stat"

SRC_A = f"rtmp://{NGINX_RTMP_HOST}:{NGINX_RTMP_PORT}/live/brb"  # looped MP4 via nginx
SRC_B = f"rtmp://{NGINX_RTMP_HOST}:{NGINX_RTMP_PORT}/live/cam"  # camera via nginx (normalized)
SRC_C = f"rtmp://{NGINX_RTMP_HOST}:{NGINX_RTMP_PORT}/live/cam-raw"  # raw camera input

# Output back to nginx and push onward from there
OUTPUT = f"rtmp://{NGINX_RTMP_HOST}:{NGINX_RTMP_PORT}/live/program"

# Monitoring configuration
STREAM_CHECK_INTERVAL = 10  # Seconds between stream availability checks
STREAM_CHECK_TIMEOUT = 2    # Timeout for nginx stats queries

def wait_for_rtmp_server(host, port, max_retries=30, retry_delay=1):
    """Wait for RTMP server to be available."""
    print(f"[init] Waiting for RTMP server at {host}:{port}...")
    for attempt in range(1, max_retries + 1):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2)
            result = sock.connect_ex((host, port))
            sock.close()
            if result == 0:
                print(f"[init] ✓ RTMP server is reachable at {host}:{port}")
                return True
        except socket.gaierror:
            pass
        except Exception as e:
            print(f"[init] Connection check error: {e}")
        
        if attempt < max_retries:
            print(f"[init] Waiting for RTMP server... (attempt {attempt}/{max_retries})")
            time.sleep(retry_delay)
    
    print(f"[init] ERROR: RTMP server at {host}:{port} not available after {max_retries} attempts")
    return False

def check_stream_exists(stream_name, app_name="live"):
    """
    Check if a stream exists on nginx-rtmp by querying stats.
    Returns dict with exists, publishing status.
    """
    try:
        req = Request(NGINX_STATS_URL, headers={"User-Agent": "rtmp-switcher/1.0"})
        with urlopen(req, timeout=STREAM_CHECK_TIMEOUT) as response:
            xml_data = response.read()
        
        root = ET.fromstring(xml_data)
        
        # Navigate: rtmp -> server -> application -> stream
        for server in root.findall(".//server"):
            for app in server.findall(".//application"):
                app_name_text = app.findtext("name", "")
                if app_name_text != app_name:
                    continue
                
                # Found the correct application
                for stream in app.findall(".//stream"):
                    stream_name_text = stream.findtext("name", "")
                    if stream_name_text != stream_name:
                        continue
                    
                    # Found the stream
                    publishing_text = stream.findtext("publishing", "")
                    nclients_text = stream.findtext("nclients", "0")
                    
                    try:
                        nclients = int(nclients_text)
                    except ValueError:
                        nclients = 0
                    
                    is_publishing = (
                        publishing_text.lower() in ("active", "1", "true", "on") or
                        nclients >= 1
                    )
                    
                    return {
                        "exists": True,
                        "publishing": is_publishing,
                        "nclients": nclients
                    }
        
        # Stream not found
        return {"exists": False, "publishing": False, "nclients": 0}
        
    except (URLError, HTTPError, ET.ParseError, Exception) as e:
        print(f"[warn] Failed to check stream {stream_name}: {e}", file=sys.stderr)
        return {"exists": False, "publishing": False, "nclients": 0}

def make(factory, name):
    e = Gst.ElementFactory.make(factory, name)
    if not e:
        print(f"[ERR] missing element: {factory}", file=sys.stderr)
    return e

def first_ok(candidates, name):
    """Try a list of element factories; return the first that exists."""
    for f in candidates:
        e = Gst.ElementFactory.make(f, name)
        if e:
            print(f"[info] using {f} for {name}")
            return e
    print(f"[FATAL] none of {candidates} available for {name}", file=sys.stderr)
    return None

class Switcher:
    def __init__(self):
        # Wait for RTMP server to be ready before building pipeline
        if not wait_for_rtmp_server(NGINX_RTMP_HOST, NGINX_RTMP_PORT, max_retries=60):
            raise RuntimeError("RTMP server not available")
        
        # Wait a bit for nginx to be fully ready
        print("[init] Waiting for nginx-rtmp to be fully ready...")
        time.sleep(2)
        
        print("[init] Building GStreamer pipeline...")
        self.pipeline = Gst.Pipeline.new("rtmp-switcher")
        
        # Track startup time for uptime calculation
        self.startup_time = datetime.now()
        
        # Track source status with detailed information
        self.source_status = {
            "brb": {
                "available": False,
                "using_fallback": False,
                "stream_exists": False,
                "last_check": None,
                "element_name": "a"
            },
            "cam": {
                "available": False,
                "using_fallback": False,
                "stream_exists": False,
                "last_check": None,
                "element_name": "b"
            },
            "cam-raw": {
                "available": False,
                "using_fallback": False,
                "stream_exists": False,
                "last_check": None,
                "element_name": "c"
            }
        }
        
        # Track which sources are available (backward compat)
        self.sources_available = {"brb": False, "cam": False, "cam-raw": False}

        def create_fallback_source(tag):
            """Create fallback test source for when real stream is unavailable."""
            print(f"[init] Creating FALLBACK source for '{tag}' (black video + silence)")
            
            # Video test source - black pattern
            vsrc = make("videotestsrc", f"{tag}_vsrc_fallback")
            vsrc.set_property("pattern", "black")
            vsrc.set_property("is-live", True)
            
            # Video processing chain
            vq = make("queue", f"{tag}_vq")
            vq.set_property("max-size-buffers", 30)
            vq.set_property("max-size-time", 0)
            vq.set_property("max-size-bytes", 0)
            vq.set_property("leaky", 1)
            vconv = make("videoconvert", f"{tag}_vconv")
            vscale = make("videoscale", f"{tag}_vscale")
            videorate = make("videorate", f"{tag}_vrate")
            vcaps = make("capsfilter", f"{tag}_vcaps")
            vcaps.set_property("caps", Gst.Caps.from_string(
                "video/x-raw,format=I420,width=1920,height=1080,framerate=30/1"
            ))
            
            # Audio test source - silence
            asrc = make("audiotestsrc", f"{tag}_asrc_fallback")
            asrc.set_property("wave", "silence")
            asrc.set_property("is-live", True)
            
            # Audio processing chain
            aq = make("queue", f"{tag}_aq")
            aq.set_property("max-size-buffers", 30)
            aq.set_property("max-size-time", 0)
            aq.set_property("max-size-bytes", 0)
            aq.set_property("leaky", 1)
            aconv = make("audioconvert", f"{tag}_aconv")
            ares = make("audioresample", f"{tag}_ares")
            acaps = make("capsfilter", f"{tag}_acaps")
            acaps.set_property("caps", Gst.Caps.from_string(
                "audio/x-raw,channels=2,rate=48000"
            ))
            
            # Add all elements to pipeline
            for e in [vsrc, vq, vconv, vscale, videorate, vcaps, asrc, aq, aconv, ares, acaps]:
                self.pipeline.add(e)
            
            # Link video chain
            vsrc.link(vq)
            vq.link(vconv)
            vconv.link(vscale)
            vscale.link(videorate)
            videorate.link(vcaps)
            
            # Link audio chain
            asrc.link(aq)
            aq.link(aconv)
            aconv.link(ares)
            ares.link(acaps)
            
            print(f"[init] ✓ Fallback source created for '{tag}'")
            
            return {
                "v_srcpad": vcaps.get_static_pad("src"),
                "a_srcpad": acaps.get_static_pad("src"),
                "is_fallback": True
            }

        # --- build two input chains (decode to raw and normalize caps) ---
        def make_input(tag, uri, stream_name, required=True):
            """
            Create input source chain with decoding and normalization.
            If stream doesn't exist and not required, creates fallback.
            """
            print(f"[init] Creating input source '{tag}' for URI: {uri}")
            
            # Check if stream exists on nginx
            stream_check = check_stream_exists(stream_name)
            stream_exists = stream_check["exists"]
            
            print(f"[init] Stream '{stream_name}' check: exists={stream_exists}, publishing={stream_check.get('publishing', False)}")
            
            # If stream doesn't exist and it's not required, use fallback
            if not stream_exists and not required:
                print(f"[init] Stream '{stream_name}' not available, using fallback source")
                return create_fallback_source(tag)
            
            # If stream doesn't exist and IS required, wait/retry
            if not stream_exists and required:
                print(f"[init] Required stream '{stream_name}' not available, waiting...")
                # Wait up to 30 seconds for required stream
                for attempt in range(1, 31):
                    time.sleep(1)
                    stream_check = check_stream_exists(stream_name)
                    if stream_check["exists"]:
                        print(f"[init] ✓ Required stream '{stream_name}' is now available")
                        stream_exists = True
                        break
                    if attempt % 5 == 0:
                        print(f"[init] Still waiting for required stream '{stream_name}'... ({attempt}/30)")
                
                if not stream_exists:
                    raise RuntimeError(f"Required stream '{stream_name}' not available after waiting")
            
            print(f"[init] Creating uridecodebin for '{tag}'")
            src = make("uridecodebin", f"{tag}_src")
            if not src:
                raise RuntimeError("uridecodebin not found (install gstreamer1.0-plugins-base)")
            src.set_property("uri", uri)
            print(f"[init] ✓ Created uridecodebin for '{tag}'")

            # video branch - configure queue for live streaming
            vq = make("queue", f"{tag}_vq")
            vq.set_property("max-size-buffers", 30)
            vq.set_property("max-size-time", 0)
            vq.set_property("max-size-bytes", 0)
            vq.set_property("leaky", 1)  # Leak upstream (drop new frames when full)
            vconv = make("videoconvert", f"{tag}_vconv")
            vscale = make("videoscale", f"{tag}_vscale")
            videorate = make("videorate", f"{tag}_vrate")
            vcaps = make("capsfilter", f"{tag}_vcaps")
            # force a stable 1080p30 raw format for clean switching/encoding
            vcaps.set_property("caps", Gst.Caps.from_string(
                "video/x-raw,format=I420,width=1920,height=1080,framerate=30/1"
            ))

            # audio branch - configure queue for live streaming
            aq = make("queue", f"{tag}_aq")
            aq.set_property("max-size-buffers", 30)
            aq.set_property("max-size-time", 0)
            aq.set_property("max-size-bytes", 0)
            aq.set_property("leaky", 1)  # Leak upstream (drop new audio when full)
            aconv = make("audioconvert", f"{tag}_aconv")
            ares = make("audioresample", f"{tag}_ares")
            acaps = make("capsfilter", f"{tag}_acaps")
            acaps.set_property("caps", Gst.Caps.from_string(
                "audio/x-raw,channels=2,rate=48000"
            ))

            for e in [src, vq, vconv, vscale, videorate, vcaps, aq, aconv, ares, acaps]:
                self.pipeline.add(e)

            # link static parts
            vq.link(vconv); vconv.link(vscale); vscale.link(videorate); videorate.link(vcaps)
            aq.link(aconv); aconv.link(ares); ares.link(acaps)

            # connect dynamic pads from uridecodebin
            def on_pad_added(element, pad):
                caps = pad.get_current_caps()
                s = caps.to_string() if caps else ""
                print(f"[pad] {tag}: New pad '{pad.get_name()}' with caps: {s[:100]}...")
                if s.startswith("video/"):
                    sink = vq.get_static_pad("sink")
                    if not sink.is_linked():
                        ret = pad.link(sink)
                        if ret == Gst.PadLinkReturn.OK:
                            print(f"[pad] {tag}: ✓ Linked video pad successfully")
                        else:
                            print(f"[pad] {tag}: ERROR: Failed to link video pad: {ret}")
                elif s.startswith("audio/"):
                    sink = aq.get_static_pad("sink")
                    if not sink.is_linked():
                        ret = pad.link(sink)
                        if ret == Gst.PadLinkReturn.OK:
                            print(f"[pad] {tag}: ✓ Linked audio pad successfully")
                        else:
                            print(f"[pad] {tag}: ERROR: Failed to link audio pad: {ret}")
            
            def on_no_more_pads(element):
                print(f"[pad] {tag}: No more pads signal received")
            
            src.connect("pad-added", on_pad_added)
            src.connect("no-more-pads", on_no_more_pads)

            return {
                "v_srcpad": vcaps.get_static_pad("src"),
                "a_srcpad": acaps.get_static_pad("src"),
                "is_fallback": False
            }

        # Create brb source (required)
        print("[init] Creating brb source (required)...")
        try:
            a = make_input("a", SRC_A, "brb", required=True)
            self.sources_available["brb"] = True
            self.source_status["brb"]["available"] = True
            self.source_status["brb"]["stream_exists"] = not a.get("is_fallback", False)
            self.source_status["brb"]["using_fallback"] = a.get("is_fallback", False)
            self.source_status["brb"]["last_check"] = datetime.now().isoformat()
            print("[init] ✓ BRB source created successfully")
        except Exception as e:
            print(f"[init] FATAL: Failed to create brb source: {e}")
            raise

        # Create cam source (optional, with fallback)
        print("[init] Creating cam source (optional)...")
        b = make_input("b", SRC_B, "cam", required=False)
        self.sources_available["cam"] = True
        self.source_status["cam"]["available"] = True
        self.source_status["cam"]["stream_exists"] = not b.get("is_fallback", False)
        self.source_status["cam"]["using_fallback"] = b.get("is_fallback", False)
        self.source_status["cam"]["last_check"] = datetime.now().isoformat()
        
        if b.get("is_fallback", False):
            print("[init] ✓ Cam source created with FALLBACK (will auto-reconnect when available)")
        else:
            print("[init] ✓ Cam source created successfully from real stream")

        # Create cam-raw source (optional, with fallback)
        print("[init] Creating cam-raw source (optional)...")
        c = make_input("c", SRC_C, "cam-raw", required=False)
        self.sources_available["cam-raw"] = True
        self.source_status["cam-raw"]["available"] = True
        self.source_status["cam-raw"]["stream_exists"] = not c.get("is_fallback", False)
        self.source_status["cam-raw"]["using_fallback"] = c.get("is_fallback", False)
        self.source_status["cam-raw"]["last_check"] = datetime.now().isoformat()
        
        if c.get("is_fallback", False):
            print("[init] ✓ Cam-raw source created with FALLBACK (will auto-reconnect when available)")
        else:
            print("[init] ✓ Cam-raw source created successfully from real stream")

        # selectors for hard, instantaneous switching
        self.vsel = make("input-selector", "vsel")
        self.asel = make("input-selector", "asel")
        for e in [self.vsel, self.asel]:
            self.pipeline.add(e)

        def link_to_selector(selector, peer_pad, label):
            sinkpad = selector.request_pad_simple("sink_%u")
            sinkpad.set_property("name", f"{label}_sink")
            ok = peer_pad.link(sinkpad)
            if ok != Gst.PadLinkReturn.OK:
                raise RuntimeError(f"failed linking {label} to selector: {ok}")
            return sinkpad

        self.a_v_sink = link_to_selector(self.vsel, a["v_srcpad"], "a_v")
        self.b_v_sink = link_to_selector(self.vsel, b["v_srcpad"], "b_v")
        self.c_v_sink = link_to_selector(self.vsel, c["v_srcpad"], "c_v")
        self.a_a_sink = link_to_selector(self.asel, a["a_srcpad"], "a_a")
        self.b_a_sink = link_to_selector(self.asel, b["a_srcpad"], "b_a")
        self.c_a_sink = link_to_selector(self.asel, c["a_srcpad"], "c_a")

        # --- encode & mux ---
        vconv2 = make("videoconvert", "vconv2")
        x264   = make("x264enc", "x264")
        x264.set_property("tune", "zerolatency")
        x264.set_property("speed-preset", "veryfast")
        x264.set_property("bitrate", 3000)     # ~3000 kbps
        x264.set_property("key-int-max", 60)   # 2s GOP @ 30 fps

        h264parse = make("h264parse", "h264parse")

        aconv2 = make("audioconvert", "aconv2")
        # Try common AAC encoders, prefer avenc_aac (libav)
        aacenc = first_ok(["avenc_aac", "fdkaacenc", "voaacenc"], "aacenc")

        aacparse = make("aacparse", "aacparse")
        if aacparse:
            # FLV needs raw AAC (no ADTS)
            try:
                aacparse.set_property("stream-format", "raw")
            except Exception:
                pass

        flv = make("flvmux", "flv")
        flv.set_property("streamable", True)

        # RTMP(S) sink: try rtmpsink first, then rtmp2sink
        sink = first_ok(["rtmpsink", "rtmp2sink", "rtmpsink"], "rtmpout")
        if not sink:
            raise RuntimeError("No RTMP sink found. Install gstreamer1.0-plugins-bad (rtmp2sink)")

        for e in [vconv2, x264, h264parse, aconv2, aacenc, aacparse, flv, sink]:
            if not e:
                raise RuntimeError("Missing a required element; check logs above.")
            self.pipeline.add(e)

        # wire video path
        self.vsel.link(vconv2)
        vconv2.link(x264); x264.link(h264parse); h264parse.link(flv)

        # wire audio path
        self.asel.link(aconv2); aconv2.link(aacenc)
        aacenc.link(aacparse); aacparse.link(flv)

        # flv -> rtmp sink
        print(f"[init] Configuring RTMP sink to publish to: {OUTPUT}")
        
        # Disable async on sink to prevent blocking pipeline state transitions
        if sink.find_property("async"):
            sink.set_property("async", False)
            print(f"[init] Set async=False on RTMP sink to prevent blocking state transitions")
        
        if sink.find_property("location"):
            print(f"[init] Using 'location' property on {sink.get_factory().get_name()}")
            sink.set_property("location", OUTPUT)
        elif sink.find_property("url"):
            print(f"[init] Using 'url' property on {sink.get_factory().get_name()}")
            sink.set_property("url", OUTPUT)
        
        if not flv.link(sink):
            raise RuntimeError("Failed to link flvmux to RTMP sink")
        print(f"[init] ✓ Successfully linked flvmux to RTMP sink")

        # bus
        self.bus = self.pipeline.get_bus()
        self.bus.add_signal_watch()
        self.bus.connect("message", self.on_bus)
        
        # Track pipeline state
        self.pipeline_playing = False
        
        # Track current active source/scene (initialize before set_source call)
        self.current_source = None
        
        # Log available sources
        print(f"[init] Available sources: {', '.join([k for k, v in self.sources_available.items() if v])}")
        
        # default program = brb
        self.set_source("brb")

    def hot_reconnect_source(self, source_name):
        """
        Replace fallback source with real stream source dynamically.
        This is called when a real stream becomes available.
        """
        print(f"[reconnect] Starting hot-reconnection for '{source_name}'")
        
        status = self.source_status[source_name]
        element_tag = status["element_name"]
        
        # Determine URI based on source
        if source_name == "brb":
            uri = SRC_A
        elif source_name == "cam":
            uri = SRC_B
        elif source_name == "cam-raw":
            uri = SRC_C
        else:
            print(f"[reconnect] ERROR: Unknown source '{source_name}'")
            return False
        
        try:
            # Step 1: Find the old fallback elements to remove
            old_vsrc = self.pipeline.get_by_name(f"{element_tag}_vsrc_fallback")
            old_asrc = self.pipeline.get_by_name(f"{element_tag}_asrc_fallback")
            
            # Step 2: Create new real source elements
            print(f"[reconnect] Creating new uridecodebin for '{source_name}'")
            new_src = make("uridecodebin", f"{element_tag}_src_new")
            new_src.set_property("uri", uri)
            
            # Video branch
            new_vq = make("queue", f"{element_tag}_vq_new")
            new_vq.set_property("max-size-buffers", 30)
            new_vq.set_property("max-size-time", 0)
            new_vq.set_property("max-size-bytes", 0)
            new_vq.set_property("leaky", 1)
            new_vconv = make("videoconvert", f"{element_tag}_vconv_new")
            new_vscale = make("videoscale", f"{element_tag}_vscale_new")
            new_vrate = make("videorate", f"{element_tag}_vrate_new")
            new_vcaps = make("capsfilter", f"{element_tag}_vcaps_new")
            new_vcaps.set_property("caps", Gst.Caps.from_string(
                "video/x-raw,format=I420,width=1920,height=1080,framerate=30/1"
            ))
            
            # Audio branch
            new_aq = make("queue", f"{element_tag}_aq_new")
            new_aq.set_property("max-size-buffers", 30)
            new_aq.set_property("max-size-time", 0)
            new_aq.set_property("max-size-bytes", 0)
            new_aq.set_property("leaky", 1)
            new_aconv = make("audioconvert", f"{element_tag}_aconv_new")
            new_ares = make("audioresample", f"{element_tag}_ares_new")
            new_acaps = make("capsfilter", f"{element_tag}_acaps_new")
            new_acaps.set_property("caps", Gst.Caps.from_string(
                "audio/x-raw,channels=2,rate=48000"
            ))
            
            # Step 3: Add new elements to pipeline (in PAUSED state initially)
            print(f"[reconnect] Adding new elements to pipeline")
            for e in [new_src, new_vq, new_vconv, new_vscale, new_vrate, new_vcaps,
                      new_aq, new_aconv, new_ares, new_acaps]:
                self.pipeline.add(e)
                e.sync_state_with_parent()
            
            # Link static parts of new chains
            new_vq.link(new_vconv)
            new_vconv.link(new_vscale)
            new_vscale.link(new_vrate)
            new_vrate.link(new_vcaps)
            
            new_aq.link(new_aconv)
            new_aconv.link(new_ares)
            new_ares.link(new_acaps)
            
            # Track pad linking completion
            pads_linked = {"video": False, "audio": False}
            link_event = threading.Event()
            
            # Step 4: Connect dynamic pad handlers for uridecodebin
            def on_pad_added(element, pad):
                caps = pad.get_current_caps()
                s = caps.to_string() if caps else ""
                print(f"[reconnect] New pad from uridecodebin: {s[:100]}...")
                
                if s.startswith("video/"):
                    sink = new_vq.get_static_pad("sink")
                    if not sink.is_linked():
                        ret = pad.link(sink)
                        if ret == Gst.PadLinkReturn.OK:
                            print(f"[reconnect] ✓ Linked video pad")
                            pads_linked["video"] = True
                            if pads_linked["audio"]:
                                link_event.set()
                        else:
                            print(f"[reconnect] ERROR: Failed to link video pad: {ret}")
                elif s.startswith("audio/"):
                    sink = new_aq.get_static_pad("sink")
                    if not sink.is_linked():
                        ret = pad.link(sink)
                        if ret == Gst.PadLinkReturn.OK:
                            print(f"[reconnect] ✓ Linked audio pad")
                            pads_linked["audio"] = True
                            if pads_linked["video"]:
                                link_event.set()
                        else:
                            print(f"[reconnect] ERROR: Failed to link audio pad: {ret}")
            
            new_src.connect("pad-added", on_pad_added)
            
            # Step 5: Set new source to PLAYING to start pad emission
            print(f"[reconnect] Starting new uridecodebin...")
            new_src.set_state(Gst.State.PLAYING)
            
            # Wait for pads to be linked (with timeout)
            print(f"[reconnect] Waiting for pads to link...")
            if not link_event.wait(timeout=10):
                print(f"[reconnect] WARNING: Timeout waiting for pads")
                # Continue anyway, might work
            
            # Step 6: Create new selector sink pads and link
            print(f"[reconnect] Linking to selector...")
            new_vsink = self.vsel.request_pad_simple("sink_%u")
            new_asink = self.asel.request_pad_simple("sink_%u")
            
            new_v_srcpad = new_vcaps.get_static_pad("src")
            new_a_srcpad = new_acaps.get_static_pad("src")
            
            if new_v_srcpad.link(new_vsink) != Gst.PadLinkReturn.OK:
                print(f"[reconnect] ERROR: Failed to link new video to selector")
                return False
            if new_a_srcpad.link(new_asink) != Gst.PadLinkReturn.OK:
                print(f"[reconnect] ERROR: Failed to link new audio to selector")
                return False
            
            print(f"[reconnect] ✓ New source linked to selector")
            
            # Step 7: Switch selector to new pads
            print(f"[reconnect] Switching selector to new source...")
            self.vsel.set_property("active-pad", new_vsink)
            self.asel.set_property("active-pad", new_asink)
            
            # Update internal tracking based on source name
            if source_name == "brb":
                self.a_v_sink = new_vsink
                self.a_a_sink = new_asink
            elif source_name == "cam":
                self.b_v_sink = new_vsink
                self.b_a_sink = new_asink
            elif source_name == "cam-raw":
                self.c_v_sink = new_vsink
                self.c_a_sink = new_asink
            
            # Step 8: Remove old fallback elements
            print(f"[reconnect] Removing old fallback elements...")
            if old_vsrc and old_asrc:
                # Get all fallback elements to remove
                old_elements = []
                for suffix in ["_vsrc_fallback", "_vq", "_vconv", "_vscale", "_vrate", "_vcaps",
                              "_asrc_fallback", "_aq", "_aconv", "_ares", "_acaps"]:
                    elem = self.pipeline.get_by_name(f"{element_tag}{suffix}")
                    if elem:
                        old_elements.append(elem)
                
                # Set to NULL and remove
                for elem in old_elements:
                    elem.set_state(Gst.State.NULL)
                    self.pipeline.remove(elem)
                
                print(f"[reconnect] ✓ Old fallback elements removed")
            
            # Step 9: Update status
            status["using_fallback"] = False
            status["stream_exists"] = True
            status["last_check"] = datetime.now().isoformat()
            
            print(f"[reconnect] ✓ Hot-reconnection complete for '{source_name}'")
            return True
            
        except Exception as e:
            print(f"[reconnect] ERROR during hot-reconnection: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
            return False

    def start_monitoring_thread(self):
        """Start background thread to monitor stream availability and handle reconnection."""
        def monitor_streams():
            print("[monitor] Background monitoring thread started")
            while True:
                try:
                    time.sleep(STREAM_CHECK_INTERVAL)
                    
                    # Check each source
                    for source_name in ["brb", "cam", "cam-raw"]:
                        status = self.source_status[source_name]
                        
                        # Check if stream exists on nginx
                        stream_check = check_stream_exists(source_name)
                        stream_exists = stream_check["exists"]
                        
                        # Update last check time
                        status["last_check"] = datetime.now().isoformat()
                        
                        # If using fallback and real stream now available
                        if status["using_fallback"] and stream_exists:
                            print(f"[monitor] Real stream '{source_name}' is now available! (was using fallback)")
                            print(f"[monitor] Initiating hot-reconnection...")
                            
                            # Perform hot-reconnection
                            success = self.hot_reconnect_source(source_name)
                            if success:
                                print(f"[monitor] ✓ Successfully reconnected '{source_name}' to real stream")
                            else:
                                print(f"[monitor] ✗ Failed to reconnect '{source_name}', will retry next interval")
                        
                        # If using real stream and it disappeared
                        elif not status["using_fallback"] and not stream_exists:
                            print(f"[monitor] WARNING: Real stream '{source_name}' is no longer available")
                            status["stream_exists"] = False
                            # Note: Pipeline continues with existing elements
                        
                        # If not using fallback and stream still exists
                        elif not status["using_fallback"] and stream_exists:
                            status["stream_exists"] = True
                
                except Exception as e:
                    print(f"[monitor] Error in monitoring thread: {e}", file=sys.stderr)
                    import traceback
                    traceback.print_exc()
        
        # Start daemon thread
        monitor_thread = threading.Thread(target=monitor_streams, daemon=True)
        monitor_thread.start()
        print("[monitor] Started stream monitoring thread")

    def on_bus(self, bus, msg):
        t = msg.type
        if t == Gst.MessageType.ERROR:
            err, dbg = msg.parse_error()
            src = msg.src.get_name() if msg.src else "unknown"
            print(f"[bus] GST ERROR from {src}: {err}", file=sys.stderr)
            print(f"[bus] Debug info: {dbg}", file=sys.stderr)
        elif t == Gst.MessageType.WARNING:
            w, dbg = msg.parse_warning()
            src = msg.src.get_name() if msg.src else "unknown"
            print(f"[bus] GST WARN from {src}: {w}", file=sys.stderr)
        elif t == Gst.MessageType.EOS:
            print("[bus] GST: EOS — source ended")
        elif t == Gst.MessageType.STATE_CHANGED:
            src_name = msg.src.get_name() if msg.src else "unknown"
            old, new, pending = msg.parse_state_changed()
            print(f"[bus] State change from {src_name}: {old.value_nick} -> {new.value_nick} (pending: {pending.value_nick})")
            if msg.src == self.pipeline:
                print(f"[pipeline] PIPELINE state changed: {old.value_nick} -> {new.value_nick}")
                if new == Gst.State.PLAYING:
                    self.pipeline_playing = True
                    print(f"[pipeline] ✓ pipeline_playing flag set to True")
        elif t == Gst.MessageType.STREAM_START:
            src_name = msg.src.get_name() if msg.src else 'unknown'
            print(f"[pipeline] Stream started from {src_name}")
        elif t == Gst.MessageType.ASYNC_DONE:
            src_name = msg.src.get_name() if msg.src else 'unknown'
            print(f"[bus] ASYNC_DONE from {src_name}")

    def set_source(self, which):
        """Switch to a different source."""
        if which not in ["brb", "cam", "cam-raw"]:
            raise ValueError(f"Unknown source: {which}. Use 'brb', 'cam', or 'cam-raw'")
        
        # Check if source is available
        if not self.sources_available.get(which, False):
            raise ValueError(f"Source '{which}' is not available")
        
        # Switch the selectors
        if which == "brb":
            self.vsel.set_property("active-pad", self.a_v_sink)
            self.asel.set_property("active-pad", self.a_a_sink)
        elif which == "cam":
            self.vsel.set_property("active-pad", self.b_v_sink)
            self.asel.set_property("active-pad", self.b_a_sink)
        elif which == "cam-raw":
            self.vsel.set_property("active-pad", self.c_v_sink)
            self.asel.set_property("active-pad", self.c_a_sink)
        
        # Update current source state
        self.current_source = which
        print(f"[switch] active = {which}")

    def start(self):
        print("[main] Starting GStreamer pipeline...")
        ret = self.pipeline.set_state(Gst.State.PLAYING)
        if ret == Gst.StateChangeReturn.FAILURE:
            print("[main] ERROR: Failed to start pipeline!", file=sys.stderr)
            return False
        print("[main] Pipeline state change initiated (will complete asynchronously)")
        
        # Wait for pipeline to reach PLAYING state
        print("[main] Waiting for pipeline to reach PLAYING state...")
        max_wait = 30  # seconds
        for attempt in range(max_wait):
            time.sleep(1)
            state = self.pipeline.get_state(0)[1]
            if state == Gst.State.PLAYING:
                print(f"[main] ✓ Pipeline reached PLAYING state after {attempt + 1}s")
                self.pipeline_playing = True
                break
            if attempt % 5 == 4:
                print(f"[main] Still waiting for PLAYING state... ({attempt + 1}/{max_wait}s, current: {state.value_nick})")
        else:
            print(f"[main] WARNING: Pipeline did not reach PLAYING state within {max_wait}s")
            state = self.pipeline.get_state(0)[1]
            print(f"[main] Current state: {state.value_nick}")
        
        # Wait a bit for output stream to start publishing
        print("[main] Waiting for output stream to start publishing...")
        time.sleep(2)
        
        # Verify output stream is being published
        output_check = check_stream_exists("program")
        if output_check["exists"]:
            print("[main] ✓ Output stream '/live/program' is being published to nginx")
        else:
            print("[main] WARNING: Output stream '/live/program' not detected on nginx yet")
            print("[main] This may resolve itself shortly, continuing...")
        
        return True

    def stop(self):
        self.pipeline.set_state(Gst.State.NULL)

switcher = Switcher()

# tiny HTTP API
class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Suppress default request logging
        pass
    
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/switch":
            src = (parse_qs(parsed.query).get("src", [""])[0] or "").lower()
            print(f"[http] Switch request received: {src}")
            try:
                switcher.set_source(src)
                self.send_response(200); self.end_headers()
                self.wfile.write(f"switched to {src}\n".encode())
            except Exception as e:
                print(f"[http] Switch error: {e}")
                self.send_response(400); self.end_headers()
                self.wfile.write(f"error: {e}\n".encode())
        elif parsed.path == "/scene":
            # Return current active scene as JSON
            try:
                scene = switcher.current_source or "brb"
                response = json.dumps({"scene": scene})
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(response.encode())
                print(f"[http] Scene query returned: {scene}")
            except Exception as e:
                print(f"[http] Scene query error: {e}")
                self.send_response(500); self.end_headers()
                self.wfile.write(f"error: {e}\n".encode())
        elif parsed.path == "/health":
            # Enhanced health check with detailed source status
            try:
                state = switcher.pipeline.get_state(0)[1]
                uptime = (datetime.now() - switcher.startup_time).total_seconds()
                
                # Build detailed health response
                health_data = {
                    "status": "healthy" if state == Gst.State.PLAYING else "starting",
                    "pipeline_state": state.value_nick,
                    "sources": {
                        "brb": {
                            "available": switcher.source_status["brb"]["available"],
                            "using_fallback": switcher.source_status["brb"]["using_fallback"],
                            "stream_exists": switcher.source_status["brb"]["stream_exists"],
                            "last_check": switcher.source_status["brb"]["last_check"]
                        },
                        "cam": {
                            "available": switcher.source_status["cam"]["available"],
                            "using_fallback": switcher.source_status["cam"]["using_fallback"],
                            "stream_exists": switcher.source_status["cam"]["stream_exists"],
                            "last_check": switcher.source_status["cam"]["last_check"]
                        },
                        "cam-raw": {
                            "available": switcher.source_status["cam-raw"]["available"],
                            "using_fallback": switcher.source_status["cam-raw"]["using_fallback"],
                            "stream_exists": switcher.source_status["cam-raw"]["stream_exists"],
                            "last_check": switcher.source_status["cam-raw"]["last_check"]
                        }
                    },
                    "output": {
                        "stream": "/live/program",
                        "publishing": switcher.pipeline_playing
                    },
                    "current_scene": switcher.current_source or "brb",
                    "uptime_seconds": int(uptime)
                }
                
                response = json.dumps(health_data, indent=2)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(response.encode())
            except Exception as e:
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                error_response = json.dumps({"status": "error", "message": str(e)})
                self.wfile.write(error_response.encode())
        else:
            self.send_response(404); self.end_headers()

def run_http():
    srv = HTTPServer(("0.0.0.0", 8088), Handler)
    print("[http] HTTP server bound to 0.0.0.0:8088")
    print("[http] Endpoints: /health, /scene, and /switch?src=brb|cam|cam-raw")
    srv.serve_forever()

if __name__ == "__main__":
    print("[main] Starting stream switcher...")
    
    # Start HTTP API server
    t = threading.Thread(target=run_http, daemon=True)
    t.start()
    print("[main] HTTP API server started")
    
    loop = GLib.MainLoop()
    try:
        if not switcher.start():
            raise RuntimeError("Failed to start pipeline")
        
        # Start monitoring thread for hot-reconnection
        switcher.start_monitoring_thread()
        
        print("[main] ✓ Stream switcher is now running")
        print("[main] ✓ Monitoring thread active - will detect stream changes")
        loop.run()
    except KeyboardInterrupt:
        print("[main] Received interrupt signal")
    except Exception as e:
        print(f"[main] ERROR: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
    finally:
        print("[main] Stopping pipeline...")
        switcher.stop()
        print("[main] Shutdown complete")