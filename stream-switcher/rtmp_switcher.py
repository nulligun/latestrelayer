#!/usr/bin/env python3
import sys, threading
from urllib.parse import urlparse, parse_qs
from http.server import BaseHTTPRequestHandler, HTTPServer

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GObject", "2.0")
from gi.repository import Gst, GObject, GLib

Gst.init(None)

# ----- CONFIG -----
SRC_A = "rtmp://nginx-rtmp:1936/live/offline"  # looped MP4 via nginx
SRC_B = "rtmp://nginx-rtmp:1936/live/cam"      # camera via nginx

# Output back to nginx and push onward from there
OUTPUT = "rtmp://nginx-rtmp:1936/live/program"

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
        self.pipeline = Gst.Pipeline.new("rtmp-switcher")

        # --- build two input chains (decode to raw and normalize caps) ---
        def make_input(tag, uri):
            src = make("uridecodebin", f"{tag}_src")
            if not src:
                raise RuntimeError("uridecodebin not found (install gstreamer1.0-plugins-base)")
            src.set_property("uri", uri)

            # video branch
            vq = make("queue", f"{tag}_vq")
            vconv = make("videoconvert", f"{tag}_vconv")
            vscale = make("videoscale", f"{tag}_vscale")
            videorate = make("videorate", f"{tag}_vrate")
            vcaps = make("capsfilter", f"{tag}_vcaps")
            # force a stable 1080p30 raw format for clean switching/encoding
            vcaps.set_property("caps", Gst.Caps.from_string(
                "video/x-raw,format=I420,width=1920,height=1080,framerate=30/1"
            ))

            # audio branch
            aq = make("queue", f"{tag}_aq")
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
            def on_pad_added(_, pad):
                caps = pad.get_current_caps()
                s = caps.to_string() if caps else ""
                if s.startswith("video/"):
                    sink = vq.get_static_pad("sink")
                    if not sink.is_linked():
                        pad.link(sink)
                elif s.startswith("audio/"):
                    sink = aq.get_static_pad("sink")
                    if not sink.is_linked():
                        pad.link(sink)
            src.connect("pad-added", on_pad_added)

            return {"v_srcpad": vcaps.get_static_pad("src"),
                    "a_srcpad": acaps.get_static_pad("src")}

        a = make_input("a", SRC_A)
        b = make_input("b", SRC_B)

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

        # RTMP(S) sink: try rtmpsink first, then rtmp2sink, then rtmpsink again (some distros only have one)
        sink = first_ok(["rtmpsink", "rtmp2sink", "rtmpsink"], "rtmpout")
        if not sink:
            raise RuntimeError("No RTMP sink found. Install gstreamer1.0-plugins-bad (rtmp2sink)")

        for e in [vconv2, x264, h264parse, aconv2, aacenc, aacparse, flv, sink]:
            if not e:
                raise RuntimeError("Missing a required element; check logs above.")
            self.pipeline.add(e)

        # wire video path
        assert self.vsel.link(vconv2) == Gst.StateChangeReturn.SUCCESS or True
        vconv2.link(x264); x264.link(h264parse); h264parse.link(flv)

        # wire audio path
        self.asel.link(aconv2); aconv2.link(aacenc)
        aacenc.link(aacparse); aacparse.link(flv)

        # flv -> rtmp sink
        if sink.find_property("location"):
            sink.set_property("location", OUTPUT)
        elif sink.find_property("url"):  # some rtmp2sink builds use 'url'
            sink.set_property("url", OUTPUT)
        flv.link(sink)

        # bus
        self.bus = self.pipeline.get_bus()
        self.bus.add_signal_watch()
        self.bus.connect("message", self.on_bus)

        # default program = offline
        self.set_source("offline")

    def on_bus(self, bus, msg):
        t = msg.type
        if t == Gst.MessageType.ERROR:
            err, dbg = msg.parse_error()
            print("GST ERROR:", err, dbg, file=sys.stderr)
        elif t == Gst.MessageType.WARNING:
            w, dbg = msg.parse_warning()
            print("GST WARN:", w, dbg, file=sys.stderr)
        elif t == Gst.MessageType.EOS:
            print("GST: EOS — source ended")

    def set_source(self, which):
        if which == "offline":
            self.vsel.set_property("active-pad", self.a_v_sink)
            self.asel.set_property("active-pad", self.a_a_sink)
        elif which == "cam":
            self.vsel.set_property("active-pad", self.b_v_sink)
            self.asel.set_property("active-pad", self.b_a_sink)
        else:
            raise ValueError("unknown source; use 'offline' or 'cam'")
        print(f"[switch] active = {which}")

    def start(self):
        self.pipeline.set_state(Gst.State.PLAYING)

    def stop(self):
        self.pipeline.set_state(Gst.State.NULL)

switcher = Switcher()

# tiny HTTP API
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/switch":
            src = (parse_qs(parsed.query).get("src", [""])[0] or "").lower()
            try:
                switcher.set_source(src)
                self.send_response(200); self.end_headers()
                self.wfile.write(f"switched to {src}\n".encode())
            except Exception as e:
                self.send_response(400); self.end_headers()
                self.wfile.write(f"error: {e}\n".encode())
        elif parsed.path == "/health":
            self.send_response(200); self.end_headers(); self.wfile.write(b"ok\n")
        else:
            self.send_response(404); self.end_headers()

def run_http():
    srv = HTTPServer(("0.0.0.0", 8088), Handler)
    print("HTTP: http://0.0.0.0:8088/switch?src=offline|cam")
    srv.serve_forever()

if __name__ == "__main__":
    t = threading.Thread(target=run_http, daemon=True)
    t.start()
    loop = GLib.MainLoop()
    try:
        switcher.start()
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        switcher.stop()