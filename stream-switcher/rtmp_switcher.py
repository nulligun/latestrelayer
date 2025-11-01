#!/usr/bin/env python3
import sys, threading, time, socket
from urllib.parse import urlparse, parse_qs
from http.server import BaseHTTPRequestHandler, HTTPServer

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
SRC_A = "rtmp://nginx-rtmp:1936/live/offline"  # looped MP4 via nginx
SRC_B = "rtmp://nginx-rtmp:1936/live/cam"      # camera via nginx

# Output back to nginx and push onward from there
OUTPUT = "rtmp://nginx-rtmp:1936/live/program"

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
        if not wait_for_rtmp_server("nginx-rtmp", 1936, max_retries=60):
            raise RuntimeError("RTMP server not available")
        
        # Wait a bit more for streams to be published
        print("[init] Waiting for streams to be published...")
        time.sleep(5)
        
        print("[init] Building GStreamer pipeline...")
        self.pipeline = Gst.Pipeline.new("rtmp-switcher")
        
        # Track which sources are available
        self.sources_available = {"offline": False, "cam": False}

        # --- build two input chains (decode to raw and normalize caps) ---
        def make_input(tag, uri):
            """Create input source chain with decoding and normalization."""
            print(f"[init] Creating input source '{tag}' for URI: {uri}")
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
                "a_srcpad": acaps.get_static_pad("src")
            }

        # Create offline source (required)
        print("[init] Creating offline source (required)...")
        try:
            a = make_input("a", SRC_A)
            self.sources_available["offline"] = True
            print("[init] ✓ Offline source created successfully")
        except Exception as e:
            print(f"[init] FATAL: Failed to create offline source: {e}")
            raise

        # Create cam source (optional, with fallback)
        print("[init] Creating cam source (optional)...")
        try:
            b = make_input("b", SRC_B)
            self.sources_available["cam"] = True
            print("[init] ✓ Cam source created successfully")
        except Exception as e:
            print(f"[init] WARNING: Failed to create cam source: {e}")
            print(f"[init] Cam will not be available for switching")
            # Use offline as fallback for cam pad
            print("[init] Using offline as fallback for cam pad")
            b = a  # Reuse offline source pads

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
        self.a_a_sink = link_to_selector(self.asel, a["a_srcpad"], "a_a")
        self.b_a_sink = link_to_selector(self.asel, b["a_srcpad"], "b_a")

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
        
        # Track current active source/scene
        self.current_source = None
        
        # Log available sources
        print(f"[init] Available sources: {', '.join([k for k, v in self.sources_available.items() if v])}")
        
        # default program = offline
        self.set_source("offline")

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
        if which not in ["offline", "cam"]:
            raise ValueError(f"Unknown source: {which}. Use 'offline' or 'cam'")
        
        # Check if source is available
        if not self.sources_available.get(which, False):
            raise ValueError(f"Source '{which}' is not available")
        
        # Switch the selectors
        if which == "offline":
            self.vsel.set_property("active-pad", self.a_v_sink)
            self.asel.set_property("active-pad", self.a_a_sink)
        elif which == "cam":
            self.vsel.set_property("active-pad", self.b_v_sink)
            self.asel.set_property("active-pad", self.b_a_sink)
        
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
                import json
                scene = switcher.current_source or "offline"
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
            # Simple health check: query actual pipeline state
            state = switcher.pipeline.get_state(0)[1]  # Get current state without blocking
            if state == Gst.State.PLAYING:
                self.send_response(200); self.end_headers()
                self.wfile.write(b"ok\n")
            else:
                self.send_response(503); self.end_headers()
                self.wfile.write(f"{state.value_nick}\n".encode())
        else:
            self.send_response(404); self.end_headers()

def run_http():
    srv = HTTPServer(("0.0.0.0", 8088), Handler)
    print("[http] HTTP server bound to 0.0.0.0:8088")
    print("[http] Endpoints: /health, /scene, and /switch?src=offline|cam")
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
        
        print("[main] ✓ Stream switcher is now running")
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