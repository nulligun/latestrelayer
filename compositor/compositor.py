#!/usr/bin/env python3
"""
GStreamer Compositor with Hybrid Architecture
Version: 2.3.0

Architecture:
- Fallback pipeline (always running): black screen + silence
- Video fallback pipeline (optional): TCP server for looping video
- SRT pipeline (dynamic): added on startup, can be removed/re-added
- Shared output (always running): compositor + audiomixer + encoders + TCP sink
- HTTP API (port 8088): scene status and privacy mode control

The pipeline starts immediately with fallback output, and SRT feed composites
over the fallback when available. When FALLBACK_SOURCE=video, a video fallback
layer is added between black screen and SRT.

New in v2.3.0:
- HTTP API for scene status and privacy mode control
- Privacy mode: prevents SRT camera from showing even when connected
- Privacy state persists across container restarts
"""
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gst, GLib
import time
import os
import json
import signal
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

Gst.init(None)

__version__ = "2.3.0"

# Environment variables
FALLBACK_SOURCE = os.getenv("FALLBACK_SOURCE", "").lower()
VIDEO_TCP_PORT = int(os.getenv("VIDEO_TCP_PORT", "1940"))
BUFFER_DELAY_MS = int(os.getenv("BUFFER_DELAY_MS", "500"))
VIDEO_WATCHDOG_TIMEOUT = float(os.getenv("VIDEO_WATCHDOG_TIMEOUT", "2.0"))
HTTP_API_PORT = int(os.getenv("HTTP_API_PORT", "8088"))
PRIVACY_STATE_FILE = "/app/privacy_state.json"
ELEMENT_REMOVAL_TIMEOUT_SEC = 2  # Timeout for state transitions during element removal

# State constants
STATE_FALLBACK_ONLY = "FALLBACK_ONLY"
STATE_VIDEO_BUFFERING = "VIDEO_BUFFERING"
STATE_VIDEO_TRANSITIONING = "VIDEO_TRANSITIONING"
STATE_VIDEO_CONNECTED = "VIDEO_CONNECTED"
STATE_SRT_BUFFERING = "SRT_BUFFERING"
STATE_SRT_TRANSITIONING = "SRT_TRANSITIONING"
STATE_SRT_CONNECTED = "SRT_CONNECTED"


class CompositorManager:
    """Manages a GStreamer compositor with hybrid architecture."""
    
    def __init__(self):
        self.pipeline = Gst.Pipeline.new("hybrid_compositor")
        
        # Element storage
        self.fallback_elements = {}
        self.output_elements = {}
        self.video_elements = None
        self.srt_elements = None
        
        # State tracking
        self.state = STATE_FALLBACK_ONLY
        self.last_video_buf_time = time.time()
        self.last_srt_buf_time = time.time()
        self.video_ever_connected = False
        self.srt_ever_connected = False
        self.video_restart_scheduled = False
        self.restart_scheduled = False
        
        # TCP connection tracking
        self.tcp_connected = False
        self.tcp_bytes_received = 0
        
        # Buffer tracking for smooth fade-in
        self.video_first_buffer_time = None
        self.srt_first_buffer_time = None
        self.video_buffer_scheduled = False
        self.srt_buffer_scheduled = False
        
        # Fade control references (compositor/audiomixer pad references)
        self.video_compositor_pad = None
        self.video_mixer_pad = None
        self.video_vol = None
        self.srt_compositor_pad = None
        self.srt_mixer_pad = None
        self.cam_vol = None
        
        # Privacy mode state
        self.privacy_enabled = False
        self._load_privacy_state()
        
        # SRT connection tracking
        self.srt_connected = False
        self.srt_bitrate_kbps = 0
        
        # MainLoop reference for signal handling
        self.loop = None
        
    def _load_privacy_state(self):
        """Load privacy state from JSON file."""
        try:
            if os.path.exists(PRIVACY_STATE_FILE):
                with open(PRIVACY_STATE_FILE, 'r') as f:
                    data = json.load(f)
                    self.privacy_enabled = data.get('enabled', False)
                    print(f"[privacy] Loaded privacy state: enabled={self.privacy_enabled}", flush=True)
            else:
                print("[privacy] No saved privacy state found, starting with privacy disabled", flush=True)
        except Exception as e:
            print(f"[privacy] Error loading privacy state: {e}", flush=True)
            self.privacy_enabled = False
    
    def _save_privacy_state(self):
        """Save privacy state to JSON file."""
        try:
            with open(PRIVACY_STATE_FILE, 'w') as f:
                json.dump({'enabled': self.privacy_enabled}, f)
            print(f"[privacy] Saved privacy state: enabled={self.privacy_enabled}", flush=True)
        except Exception as e:
            print(f"[privacy] Error saving privacy state: {e}", flush=True)
    
    def set_privacy_mode(self, enabled):
        """Enable or disable privacy mode."""
        self.privacy_enabled = enabled
        self._save_privacy_state()
        
        if enabled:
            print("[privacy] Privacy mode ENABLED - SRT camera will not be shown", flush=True)
            # If SRT is currently showing, fade it out
            if self.state in (STATE_SRT_CONNECTED, STATE_SRT_TRANSITIONING):
                print("[privacy] Fading out SRT camera due to privacy mode activation", flush=True)
                self.start_fade_out("srt")
        else:
            print("[privacy] Privacy mode DISABLED - normal camera operation resumed", flush=True)
    
    def get_current_scene(self):
        """Get the current scene name based on compositor state."""
        if self.state == STATE_SRT_CONNECTED or self.state == STATE_SRT_TRANSITIONING:
            return "SRT"
        elif self.state == STATE_VIDEO_CONNECTED or self.state == STATE_VIDEO_TRANSITIONING:
            return "VIDEO"
        else:
            return "BLACK"
    
    def get_srt_stats(self):
        """Get SRT connection statistics including bitrate."""
        if not self.srt_elements or not self.srt_connected:
            return {'connected': False, 'bitrate_kbps': 0}
        
        try:
            srt_src = self.srt_elements.get('srt_src')
            if srt_src:
                stats = srt_src.get_property("stats")
                if stats:
                    # Extract bitrate from stats structure
                    # SRT stats typically contain mbps-send-rate or similar
                    bitrate_bps = stats.get('bitrate', 0)
                    bitrate_kbps = int(bitrate_bps / 1024) if bitrate_bps else 0
                    return {'connected': True, 'bitrate_kbps': bitrate_kbps}
        except Exception as e:
            print(f"[srt-stats] Error getting SRT stats: {e}", flush=True)
        
        return {'connected': self.srt_connected, 'bitrate_kbps': 0}
    
    def get_tcp_stats(self):
        """Get TCP connection statistics including bytes received."""
        if not self.video_elements:
            return {'connected': False, 'bytes_received': 0, 'current_port': 0}
        
        try:
            tcp_src = self.video_elements.get('tcp_src')
            if tcp_src:
                stats = tcp_src.get_property("stats")
                current_port = tcp_src.get_property("current-port")
                if stats:
                    print(f"[tcp-stats] TCP connection stats: {stats}", flush=True)
                    return {'connected': True, 'stats': str(stats), 'current_port': current_port}
                else:
                    return {'connected': False, 'bytes_received': 0, 'current_port': current_port}
        except Exception as e:
            print(f"[tcp-stats] Error getting TCP stats: {e}", flush=True)
        
        return {'connected': False, 'bytes_received': 0, 'current_port': 0}
    
    def _build_fallback_sources(self):
        """Create always-running fallback sources (black + silence)."""
        print("[build] Creating fallback sources (black + silence)...", flush=True)
        
        # Black video source
        black_src = Gst.ElementFactory.make("videotestsrc", "black_src")
        black_src.set_property("pattern", 2)  # black
        black_src.set_property("is-live", True)
        black_src.set_property("do-timestamp", True)
        
        black_capsfilter = Gst.ElementFactory.make("capsfilter", "black_caps")
        black_caps = Gst.Caps.from_string("video/x-raw,width=1920,height=1080,framerate=30/1")
        black_capsfilter.set_property("caps", black_caps)
        
        black_vconv = Gst.ElementFactory.make("videoconvert", "black_vconv")
        
        # Silent audio source
        silence_src = Gst.ElementFactory.make("audiotestsrc", "silence_src")
        silence_src.set_property("wave", 4)  # silence
        silence_src.set_property("is-live", True)
        silence_src.set_property("do-timestamp", True)
        silence_src.set_property("volume", 0.0)
        
        silence_capsfilter = Gst.ElementFactory.make("capsfilter", "silence_caps")
        silence_caps = Gst.Caps.from_string("audio/x-raw,rate=48000,channels=2")
        silence_capsfilter.set_property("caps", silence_caps)
        
        silence_aconv = Gst.ElementFactory.make("audioconvert", "silence_aconv")
        silence_ares = Gst.ElementFactory.make("audioresample", "silence_ares")
        
        self.fallback_elements = {
            'black_src': black_src,
            'black_capsfilter': black_capsfilter,
            'black_vconv': black_vconv,
            'silence_src': silence_src,
            'silence_capsfilter': silence_capsfilter,
            'silence_aconv': silence_aconv,
            'silence_ares': silence_ares,
        }
        
        # Add to pipeline
        for elem in self.fallback_elements.values():
            self.pipeline.add(elem)
        
        # Link fallback video chain
        black_src.link(black_capsfilter)
        black_capsfilter.link(black_vconv)
        
        # Link fallback audio chain
        silence_src.link(silence_capsfilter)
        silence_capsfilter.link(silence_aconv)
        silence_aconv.link(silence_ares)
        
        print("[build] ✓ Fallback sources created", flush=True)
    
    def _build_output_stage(self):
        """Create always-running output stage (compositor + mixer + encoders + TCP)."""
        print("[build] Creating shared output stage...", flush=True)
        
        # Video compositor
        compositor = Gst.ElementFactory.make("compositor", "comp")
        compositor.set_property("background", 1)  # black background
        
        # Audio mixer
        audiomixer = Gst.ElementFactory.make("audiomixer", "amix")
        
        # Video encoding chain
        vconv_out = Gst.ElementFactory.make("videoconvert", "vconv_out")
        x264enc = Gst.ElementFactory.make("x264enc", "x264")
        x264enc.set_property("tune", "zerolatency")
        x264enc.set_property("speed-preset", "superfast")
        x264enc.set_property("bitrate", 2500)
        x264enc.set_property("key-int-max", 60)
        
        print(f"[build] x264enc config: tune=zerolatency, preset=superfast, bitrate=2500, key-int-max=60", flush=True)
        
        video_mux_queue = Gst.ElementFactory.make("queue", "video_mux_q")
        
        # Audio encoding
        aacenc = Gst.ElementFactory.make("avenc_aac", "aac")
        aacenc.set_property("bitrate", 128000)
        
        audio_mux_queue = Gst.ElementFactory.make("queue", "audio_mux_q")
        
        # Muxer and output
        mpegtsmux = Gst.ElementFactory.make("mpegtsmux", "mux")
        mpegtsmux.set_property("alignment", 7)
        
        print(f"[build] mpegtsmux config: alignment=7", flush=True)
        
        tcp_sink = Gst.ElementFactory.make("tcpserversink", "tcp_sink")
        tcp_sink.set_property("host", "0.0.0.0")
        tcp_sink.set_property("port", 5000)
        tcp_sink.set_property("sync-method", 0)
        tcp_sink.set_property("recover-policy", 0)
        
        self.output_elements = {
            'compositor': compositor,
            'audiomixer': audiomixer,
            'vconv_out': vconv_out,
            'x264enc': x264enc,
            'video_mux_queue': video_mux_queue,
            'aacenc': aacenc,
            'audio_mux_queue': audio_mux_queue,
            'mpegtsmux': mpegtsmux,
            'tcp_sink': tcp_sink,
        }
        
        # Add to pipeline
        for elem in self.output_elements.values():
            self.pipeline.add(elem)
        
        # Link video encoding chain
        compositor.link(vconv_out)
        vconv_out.link(x264enc)
        x264enc.link(video_mux_queue)
        
        # Link TCP sink to muxer
        mpegtsmux.link(tcp_sink)
        
        print("[build] ✓ Output stage created", flush=True)
    
    def _link_fallback_to_output(self):
        """Link fallback sources to compositor and audiomixer."""
        print("[build] Linking fallback to output...", flush=True)
        
        compositor = self.output_elements['compositor']
        audiomixer = self.output_elements['audiomixer']
        
        # Link fallback video to compositor
        black_vconv = self.fallback_elements['black_vconv']
        black_pad = compositor.request_pad_simple("sink_%u")
        black_vconv_src = black_vconv.get_static_pad("src")
        video_link_result = black_vconv_src.link(black_pad)
        print(f"[build] Fallback video → compositor link: {video_link_result}", flush=True)
        
        # Link fallback audio to audiomixer
        silence_ares = self.fallback_elements['silence_ares']
        silence_pad = audiomixer.request_pad_simple("sink_%u")
        silence_ares_src = silence_ares.get_static_pad("src")
        audio_link_result = silence_ares_src.link(silence_pad)
        print(f"[build] Fallback audio → audiomixer link: {audio_link_result}", flush=True)
        
        # Link audiomixer to aacenc
        aacenc = self.output_elements['aacenc']
        audio_mux_queue = self.output_elements['audio_mux_queue']
        mixer_link_result = audiomixer.link(aacenc)
        print(f"[build] Audiomixer → aacenc link: {mixer_link_result}", flush=True)
        aacenc.link(audio_mux_queue)
        
        # Link queues to muxer
        mpegtsmux = self.output_elements['mpegtsmux']
        video_mux_queue = self.output_elements['video_mux_queue']
        
        video_to_mux = video_mux_queue.link(mpegtsmux)
        audio_to_mux = audio_mux_queue.link(mpegtsmux)
        print(f"[build] video_queue → mux link: {video_to_mux}", flush=True)
        print(f"[build] audio_queue → mux link: {audio_to_mux}", flush=True)
        
        print("[build] ✓ Fallback linked to output", flush=True)
    
    def add_video_elements(self):
        """Add TCP video server elements to the running pipeline."""
        if self.video_elements:
            print("[video] Video elements already added", flush=True)
            return
        
        print("[video] Adding video TCP server elements to pipeline...", flush=True)
        
        # Create TCP video server chain
        tcp_src = Gst.ElementFactory.make("tcpserversrc", "tcp_src")
        tcp_src.set_property("host", "0.0.0.0")
        tcp_src.set_property("port", VIDEO_TCP_PORT)
        tcp_src.set_property("do-timestamp", True)
        
        print(f"[tcp-debug] tcpserversrc created, host=0.0.0.0, port={VIDEO_TCP_PORT}, do-timestamp=True", flush=True)
        
        # Add debug signal handlers for TCP connection events
        def on_client_added(element, socket_fd):
            print(f"[tcp-connection] ✓ CLIENT CONNECTED at socket level! fd={socket_fd}", flush=True)
            self.tcp_connected = True
        
        def on_client_removed(element, socket_fd, status):
            print(f"[tcp-connection] ✗ CLIENT DISCONNECTED at socket level! fd={socket_fd}, status={status}", flush=True)
            self.tcp_connected = False
            self.tcp_bytes_received = 0
        
        # Connect TCP server signals if available
        try:
            tcp_src.connect("client-added", on_client_added)
            print("[tcp-debug] ✓ Connected to 'client-added' signal", flush=True)
        except Exception as e:
            print(f"[tcp-debug] Could not connect 'client-added' signal: {e}", flush=True)
        
        try:
            tcp_src.connect("client-removed", on_client_removed)
            print("[tcp-debug] ✓ Connected to 'client-removed' signal", flush=True)
        except Exception as e:
            print(f"[tcp-debug] Could not connect 'client-removed' signal: {e}", flush=True)
        
        decode = Gst.ElementFactory.make("decodebin", "video_decode")
        
        # Add debug signal handlers for decodebin
        def on_decode_unknown_type(element, pad, caps):
            print(f"[tcp-debug] decodebin unknown-type: {caps.to_string()}", flush=True)
        
        def on_decode_autoplug_continue(element, pad, caps):
            print(f"[tcp-debug] decodebin autoplug-continue: {caps.to_string()}", flush=True)
            return True
        
        def on_decode_autoplug_select(element, pad, caps, factory):
            print(f"[tcp-debug] decodebin autoplug-select: caps={caps.to_string()}, factory={factory.get_name()}", flush=True)
            return 0  # GST_AUTOPLUG_SELECT_TRY
        
        decode.connect("unknown-type", on_decode_unknown_type)
        decode.connect("autoplug-continue", on_decode_autoplug_continue)
        decode.connect("autoplug-select", on_decode_autoplug_select)
        
        # Video chain with improved buffering
        video_queue = Gst.ElementFactory.make("queue", "video_video_q")
        video_queue.set_property("max-size-time", 3000000000)  # 3 seconds (improved from 1s)
        video_queue.set_property("max-size-buffers", 0)
        video_queue.set_property("leaky", 2)
        videoconvert = Gst.ElementFactory.make("videoconvert", "video_vconv")
        videoscale = Gst.ElementFactory.make("videoscale", "video_vscale")
        videorate = Gst.ElementFactory.make("videorate", "video_vrate")
        videorate.set_property("drop-only", True)
        
        # Capsfilter for explicit format negotiation (no framerate constraint - videorate handles that)
        video_capsfilter = Gst.ElementFactory.make("capsfilter", "video_caps")
        video_caps = Gst.Caps.from_string("video/x-raw,width=1920,height=1080")
        video_capsfilter.set_property("caps", video_caps)
        
        # Queue before compositor to prevent blocking
        video_comp_queue = Gst.ElementFactory.make("queue", "video_comp_q")
        video_comp_queue.set_property("max-size-time", 500000000)  # 500ms
        video_comp_queue.set_property("max-size-buffers", 0)
        video_comp_queue.set_property("leaky", 2)  # Drop old buffers to prevent pipeline blocking
        
        # Audio chain
        audio_queue = Gst.ElementFactory.make("queue", "video_audio_q")
        audio_queue.set_property("max-size-time", 3000000000)  # 3 seconds (matched with video)
        audio_queue.set_property("max-size-buffers", 0)
        audioconvert = Gst.ElementFactory.make("audioconvert", "video_aconv")
        audioresample = Gst.ElementFactory.make("audioresample", "video_ares")
        
        self.video_vol = Gst.ElementFactory.make("volume", "video_vol")
        self.video_vol.set_property("volume", 0.0)
        
        self.video_elements = {
            'tcp_src': tcp_src,
            'decode': decode,
            'video_queue': video_queue,
            'videoconvert': videoconvert,
            'videoscale': videoscale,
            'videorate': videorate,
            'video_capsfilter': video_capsfilter,
            'video_comp_queue': video_comp_queue,
            'audio_queue': audio_queue,
            'audioconvert': audioconvert,
            'audioresample': audioresample,
            'video_vol': self.video_vol,
        }
        
        # Add to pipeline
        for elem in self.video_elements.values():
            self.pipeline.add(elem)
        
        # Link video chain with detailed logging
        link_result = tcp_src.link(decode)
        print(f"[tcp-debug] tcp_src → decode link result: {link_result}", flush=True)
        
        # Add source pad probe to detect TCP data flow
        tcp_src_pad = tcp_src.get_static_pad("src")
        print(f"[tcp-debug] tcpserversrc 'src' pad lookup result: {tcp_src_pad}", flush=True)
        
        if tcp_src_pad:
            # Check pad direction and availability
            pad_direction = tcp_src_pad.get_direction()
            pad_caps = tcp_src_pad.get_current_caps()
            print(f"[tcp-debug] Pad direction: {pad_direction}, caps: {pad_caps}", flush=True)
            
            def on_tcp_src_probe(pad, info):
                buffer = info.get_buffer()
                self.tcp_bytes_received += buffer.get_size()
                print(f"[tcp-probe] ✓ DATA FLOWING from tcpserversrc! First bytes received from client, buffer_size={buffer.get_size()}, total_bytes={self.tcp_bytes_received}", flush=True)
                # Only log once
                return Gst.PadProbeReturn.REMOVE
            
            probe_id = tcp_src_pad.add_probe(Gst.PadProbeType.BUFFER, on_tcp_src_probe)
            print(f"[tcp-debug] ✓ Added probe (id={probe_id}) to tcpserversrc pad to detect data flow", flush=True)
        else:
            print(f"[tcp-debug] ⚠ WARNING: Could not get 'src' pad from tcpserversrc!", flush=True)
        
        # Add decode sink pad probe to confirm decodebin is receiving data
        def on_decode_sink_probe(pad, info):
            buffer = info.get_buffer()
            print(f"[tcp-probe] ✓ DATA RECEIVED by decodebin! Decoding in progress, buffer_size={buffer.get_size()}", flush=True)
            return Gst.PadProbeReturn.REMOVE
        
        decode_sink = decode.get_static_pad("sink")
        print(f"[tcp-debug] decodebin 'sink' pad lookup result: {decode_sink}", flush=True)
        
        if decode_sink:
            probe_id = decode_sink.add_probe(Gst.PadProbeType.BUFFER, on_decode_sink_probe)
            print(f"[tcp-debug] ✓ Added probe (id={probe_id}) to decodebin sink pad", flush=True)
            
            # Check decode sink pad state
            peer = decode_sink.get_peer()
            print(f"[tcp-debug] decodebin sink pad peer: {peer.get_parent().get_name() if peer else 'None'}", flush=True)
        else:
            print(f"[tcp-debug] ⚠ WARNING: Could not get 'sink' pad from decodebin!", flush=True)
        
        video_queue.link(videoconvert)
        videoconvert.link(videoscale)
        videoscale.link(videorate)
        videorate.link(video_capsfilter)
        video_capsfilter.link(video_comp_queue)
        
        # Link audio chain
        audio_queue.link(audioconvert)
        audioconvert.link(audioresample)
        audioresample.link(self.video_vol)
        
        # Connect signals
        decode.connect("pad-added", self._on_video_pad_added)
        video_queue_src = video_queue.get_static_pad("src")
        video_queue_src.add_probe(Gst.PadProbeType.BUFFER, self._on_video_probe)
        
        # Sync state
        for elem in self.video_elements.values():
            elem.sync_state_with_parent()
        
        print(f"[video] ✓ Video elements added, TCP server listening on port {VIDEO_TCP_PORT}", flush=True)
    
    def _on_video_pad_added(self, decodebin, pad):
        """Handle dynamic pad creation from decodebin when TCP video connects."""
        caps = pad.get_current_caps()
        name = caps.to_string() if caps else ""
        print(f"[video] Decodebin pad added: {name}", flush=True)
        
        if not self.video_ever_connected:
            self.video_ever_connected = True
            print("[video] ✓ First TCP video connection successful", flush=True)
        
        if name.startswith("video/"):
            sinkpad = self.video_elements['video_queue'].get_static_pad("sink")
            if not sinkpad.is_linked():
                ret = pad.link(sinkpad)
                print(f"[video] Video pad link result: {ret}", flush=True)
                
                compositor = self.output_elements['compositor']
                self.video_compositor_pad = compositor.request_pad_simple("sink_%u")
                self.video_compositor_pad.set_property("alpha", 0.0)
                video_comp_queue = self.video_elements['video_comp_queue']
                comp_queue_src = video_comp_queue.get_static_pad("src")
                comp_queue_src.link(self.video_compositor_pad)
                print(f"[video] ✓ Video linked to compositor pad (alpha=0.0)", flush=True)
                
        elif name.startswith("audio/"):
            sinkpad = self.video_elements['audio_queue'].get_static_pad("sink")
            if not sinkpad.is_linked():
                ret = pad.link(sinkpad)
                print(f"[video] Audio pad link result: {ret}", flush=True)
                
                audiomixer = self.output_elements['audiomixer']
                self.video_mixer_pad = audiomixer.request_pad_simple("sink_%u")
                vol_src = self.video_vol.get_static_pad("src")
                vol_src.link(self.video_mixer_pad)
                print("[video] ✓ Audio linked to audiomixer", flush=True)
    
    def _on_video_probe(self, pad, info):
        """Track TCP video buffer flow and trigger buffering/fade-in."""
        self.last_video_buf_time = time.time()
        
        if self.state == STATE_VIDEO_TRANSITIONING:
            buffer = info.get_buffer()
            alpha_val = self.video_compositor_pad.get_property("alpha") if self.video_compositor_pad else 0.0
            print(f"[probe] VIDEO_TRANSITIONING: buffer flowing, pad_alpha={alpha_val:.2f}, pts={buffer.pts/Gst.SECOND:.3f}s", flush=True)
        
        # Start buffering on first buffer
        if self.state == STATE_FALLBACK_ONLY:
            if self.video_first_buffer_time is None:
                self.video_first_buffer_time = time.time()
                self.state = STATE_VIDEO_BUFFERING
                print(f"[buffer] Video buffering started, waiting {BUFFER_DELAY_MS}ms before fade-in...", flush=True)
                # Schedule fade-in after buffer delay
                if not self.video_buffer_scheduled:
                    self.video_buffer_scheduled = True
                    GLib.timeout_add(BUFFER_DELAY_MS, self._start_video_fade_after_buffer)
        
        return Gst.PadProbeReturn.OK
    
    def remove_video_elements(self):
        """Remove video TCP elements from the pipeline with robust error handling."""
        if not self.video_elements:
            print("[video] No video elements to remove", flush=True)
            return
        
        print("[video] Removing video TCP elements from pipeline...", flush=True)
        
        # Track removal progress
        failed_elements = []
        
        for elem_name, elem in self.video_elements.items():
            try:
                print(f"[video] Attempting to remove element: {elem_name}", flush=True)
                
                # Set element to NULL state with timeout to prevent hanging
                elem.set_state(Gst.State.NULL)
                
                # Wait for state change with timeout (prevents infinite blocking)
                ret, state, pending = elem.get_state(ELEMENT_REMOVAL_TIMEOUT_SEC * Gst.SECOND)
                
                if ret == Gst.StateChangeReturn.FAILURE:
                    print(f"[video] ⚠ State transition to NULL failed for {elem_name}, attempting removal anyway", flush=True)
                elif ret == Gst.StateChangeReturn.ASYNC:
                    print(f"[video] ⚠ State transition timeout for {elem_name} (timeout={ELEMENT_REMOVAL_TIMEOUT_SEC}s), forcing removal", flush=True)
                else:
                    print(f"[video] ✓ State transition successful for {elem_name}", flush=True)
                
                # Attempt to remove from pipeline
                try:
                    self.pipeline.remove(elem)
                    print(f"[video] ✓ Element {elem_name} removed from pipeline", flush=True)
                except Exception as remove_err:
                    print(f"[video] ⚠ Failed to remove {elem_name} from pipeline: {remove_err}", flush=True)
                    failed_elements.append(elem_name)
                    
            except Exception as e:
                print(f"[video] ⚠ Exception while removing {elem_name}: {e}", flush=True)
                failed_elements.append(elem_name)
                # Continue with next element instead of failing completely
                continue
        
        # Clear references regardless of individual failures
        self.video_elements = None
        self.video_compositor_pad = None
        self.video_mixer_pad = None
        self.video_vol = None
        
        if failed_elements:
            print(f"[video] ⚠ Video element removal completed with errors for: {', '.join(failed_elements)}", flush=True)
        else:
            print("[video] ✓ Video elements removed successfully", flush=True)
    
    def restart_video_elements(self):
        """Restart video TCP elements after a timeout."""
        print("[video] Restarting video TCP server...", flush=True)
        self.video_restart_scheduled = False
        
        # Remove elements with proper state transition waiting
        self.remove_video_elements()
        
        # Reset ALL buffer tracking state to prevent stale state
        self.video_first_buffer_time = None
        self.video_buffer_scheduled = False
        print("[video] Reset buffer tracking state", flush=True)
        
        # Ensure state is reset to appropriate state
        if self.state not in (STATE_FALLBACK_ONLY, STATE_SRT_CONNECTED, STATE_SRT_TRANSITIONING):
            self.state = STATE_FALLBACK_ONLY
            print(f"[video] Reset state to {self.state}", flush=True)
        
        # CRITICAL FIX: Wait 500ms for OS to release TCP port 1940
        # The port may be in TIME_WAIT state after closing the previous connection
        def delayed_add():
            print("[video] Port release delay complete, recreating TCP server...", flush=True)
            self.add_video_elements()
            print("[video] ✓ Video TCP server restart complete, waiting for connection...", flush=True)
            return False
        
        print("[video] Waiting 500ms for port release...", flush=True)
        GLib.timeout_add(500, delayed_add)
        return False
    
    def video_watchdog_cb(self):
        """Check if TCP video has stopped sending data."""
        now = time.time()
        delta = now - self.last_video_buf_time
        
        if self.state in (STATE_VIDEO_CONNECTED, STATE_VIDEO_TRANSITIONING, STATE_VIDEO_BUFFERING) and delta > VIDEO_WATCHDOG_TIMEOUT:
            if self.state == STATE_VIDEO_BUFFERING:
                print(f"[watchdog] No video data during buffering for {delta:.1f}s (timeout={VIDEO_WATCHDOG_TIMEOUT}s), cancelling buffer", flush=True)
                self.state = STATE_FALLBACK_ONLY
                self.video_first_buffer_time = None
                self.video_buffer_scheduled = False
            else:
                print(f"[watchdog] No video TCP data for {delta:.1f}s (timeout={VIDEO_WATCHDOG_TIMEOUT}s), state={self.state}, triggering fade-out", flush=True)
                self.start_fade_out("video")
        
        return True
    
    def add_srt_elements(self):
        """Add SRT source elements to the running pipeline."""
        if self.srt_elements:
            print("[srt] SRT elements already added", flush=True)
            return
        
        print("[srt] Adding SRT elements to pipeline...", flush=True)
        
        srt_src = Gst.ElementFactory.make("srtserversrc", "srt_src")
        srt_src.set_property("uri", "srt://:1937?mode=listener")
        srt_src.set_property("latency", 2000)
        
        print(f"[srt-debug] srtserversrc created, uri=srt://:1937?mode=listener, latency=2000", flush=True)
        
        # Add signal handlers for SRT connection events
        def on_caller_connected(element, socket_fd, addr):
            print(f"[srt-connection] ✓ CLIENT CONNECTED at socket level! fd={socket_fd}, addr={addr}", flush=True)
            self.srt_connected = True
        
        def on_caller_disconnected(element, socket_fd, addr):
            print(f"[srt-connection] ✗ CLIENT DISCONNECTED at socket level! fd={socket_fd}, addr={addr}", flush=True)
            self.srt_connected = False
            self.srt_bitrate_kbps = 0
        
        # Connect signals if they exist (srtserversrc supports these)
        try:
            srt_src.connect("caller-added", on_caller_connected)
            print("[srt-debug] ✓ Connected to 'caller-added' signal", flush=True)
        except Exception as e:
            print(f"[srt-debug] Could not connect 'caller-added' signal: {e}", flush=True)
        
        try:
            srt_src.connect("caller-removed", on_caller_disconnected)
            print("[srt-debug] ✓ Connected to 'caller-removed' signal", flush=True)
        except Exception as e:
            print(f"[srt-debug] Could not connect 'caller-removed' signal: {e}", flush=True)
        
        decode = Gst.ElementFactory.make("decodebin", "decode")
        
        # Add debug signal handlers
        def on_decode_unknown_type(element, pad, caps):
            print(f"[srt-debug] decodebin unknown-type: {caps.to_string()}", flush=True)
        
        def on_decode_autoplug_continue(element, pad, caps):
            print(f"[srt-debug] decodebin autoplug-continue: {caps.to_string()}", flush=True)
            return True
        
        def on_decode_autoplug_select(element, pad, caps, factory):
            print(f"[srt-debug] decodebin autoplug-select: caps={caps.to_string()}, factory={factory.get_name()}", flush=True)
            return 0  # GST_AUTOPLUG_SELECT_TRY
        
        decode.connect("unknown-type", on_decode_unknown_type)
        decode.connect("autoplug-continue", on_decode_autoplug_continue)
        decode.connect("autoplug-select", on_decode_autoplug_select)
        
        video_queue = Gst.ElementFactory.make("queue", "video_q")
        video_queue.set_property("max-size-time", 1000000000)
        video_queue.set_property("max-size-buffers", 0)
        video_queue.set_property("leaky", 2)
        videoconvert = Gst.ElementFactory.make("videoconvert", "vconv")
        videoscale = Gst.ElementFactory.make("videoscale", "vscale")
        videorate = Gst.ElementFactory.make("videorate", "vrate")
        videorate.set_property("drop-only", True)
        
        audio_queue = Gst.ElementFactory.make("queue", "audio_q")
        audio_queue.set_property("max-size-time", 2000000000)
        audio_queue.set_property("max-size-buffers", 0)
        audioconvert = Gst.ElementFactory.make("audioconvert", "aconv")
        audioresample = Gst.ElementFactory.make("audioresample", "ares")
        
        self.cam_vol = Gst.ElementFactory.make("volume", "cam_vol")
        self.cam_vol.set_property("volume", 0.0)
        
        self.srt_elements = {
            'srt_src': srt_src,
            'decode': decode,
            'video_queue': video_queue,
            'videoconvert': videoconvert,
            'videoscale': videoscale,
            'videorate': videorate,
            'audio_queue': audio_queue,
            'audioconvert': audioconvert,
            'audioresample': audioresample,
            'cam_vol': self.cam_vol,
        }
        
        for elem in self.srt_elements.values():
            self.pipeline.add(elem)
        
        # Link with detailed logging
        link_result = srt_src.link(decode)
        print(f"[srt-debug] srt_src → decode link result: {link_result}", flush=True)
        
        # Add source pad probe to detect data flow
        srt_src_pad = srt_src.get_static_pad("src")
        print(f"[srt-debug] srtserversrc 'src' pad lookup result: {srt_src_pad}", flush=True)
        
        if srt_src_pad:
            # Check pad direction and availability
            pad_direction = srt_src_pad.get_direction()
            pad_caps = srt_src_pad.get_current_caps()
            print(f"[srt-debug] Pad direction: {pad_direction}, caps: {pad_caps}", flush=True)
            
            def on_srt_src_probe(pad, info):
                print(f"[srt-probe] ✓ DATA FLOWING from srtserversrc! First bytes received from client", flush=True)
                # Only log once
                return Gst.PadProbeReturn.REMOVE
            
            probe_id = srt_src_pad.add_probe(Gst.PadProbeType.BUFFER, on_srt_src_probe)
            print(f"[srt-debug] ✓ Added probe (id={probe_id}) to srtserversrc pad to detect data flow", flush=True)
        else:
            print(f"[srt-debug] ⚠ WARNING: Could not get 'src' pad from srtserversrc!", flush=True)
            # Try to list all pads
            pads_iterator = srt_src.iterate_pads()
            has_pads, pad = pads_iterator.next()
            if has_pads == Gst.IteratorResult.OK:
                print(f"[srt-debug] Available pads: {pad.get_name()}", flush=True)
            else:
                print(f"[srt-debug] No pads found on srtserversrc", flush=True)
        
        # Add SRT statistics monitoring
        def check_srt_stats():
            try:
                stats = srt_src.get_property("stats")
                print(f"[srt-stats] SRT connection stats: {stats}", flush=True)
            except Exception as e:
                print(f"[srt-stats] Could not get stats: {e}", flush=True)
            return True
        
        GLib.timeout_add_seconds(2, check_srt_stats)
        video_queue.link(videoconvert)
        videoconvert.link(videoscale)
        videoscale.link(videorate)
        audio_queue.link(audioconvert)
        audioconvert.link(audioresample)
        audioresample.link(self.cam_vol)
        
        decode.connect("pad-added", self._on_srt_pad_added)
        
        # Add decode source pad probe to confirm decodebin is receiving data
        def on_decode_sink_probe(pad, info):
            print(f"[srt-probe] ✓ DATA RECEIVED by decodebin! Decoding in progress...", flush=True)
            return Gst.PadProbeReturn.REMOVE
        
        decode_sink = decode.get_static_pad("sink")
        print(f"[srt-debug] decodebin 'sink' pad lookup result: {decode_sink}", flush=True)
        
        if decode_sink:
            probe_id = decode_sink.add_probe(Gst.PadProbeType.BUFFER, on_decode_sink_probe)
            print(f"[srt-debug] ✓ Added probe (id={probe_id}) to decodebin sink pad", flush=True)
            
            # Check decode sink pad state
            peer = decode_sink.get_peer()
            print(f"[srt-debug] decodebin sink pad peer: {peer.get_parent().get_name() if peer else 'None'}", flush=True)
        else:
            print(f"[srt-debug] ⚠ WARNING: Could not get 'sink' pad from decodebin!", flush=True)
        video_queue_src = video_queue.get_static_pad("src")
        video_queue_src.add_probe(Gst.PadProbeType.BUFFER, self._on_srt_video_probe)
        
        # Sync states for all elements
        for elem in self.srt_elements.values():
            state_result = elem.sync_state_with_parent()
            elem_name = elem.get_name()
            current_state = elem.get_state(0)
            print(f"[srt-debug] {elem_name} sync_state result: {state_result}, state: {current_state[1]}", flush=True)
        
        # CRITICAL FIX: Explicitly set decodebin state to PLAYING
        # decodebin often gets stuck in READY when dynamically added
        print("[srt-fix] Explicitly setting decodebin to PLAYING state...", flush=True)
        decode_set_result = decode.set_state(Gst.State.PLAYING)
        print(f"[srt-fix] decodebin set_state(PLAYING) result: {decode_set_result}", flush=True)
        
        # Wait for state change to complete
        ret, state, pending = decode.get_state(2 * Gst.SECOND)
        print(f"[srt-fix] decodebin get_state after explicit set: ret={ret}, state={state}, pending={pending}", flush=True)
        
        # Also ensure srtserversrc is in PLAYING
        print("[srt-fix] Explicitly setting srtserversrc to PLAYING state...", flush=True)
        srt_set_result = srt_src.set_state(Gst.State.PLAYING)
        print(f"[srt-fix] srtserversrc set_state(PLAYING) result: {srt_set_result}", flush=True)
        
        ret, state, pending = srt_src.get_state(2 * Gst.SECOND)
        print(f"[srt-fix] srtserversrc get_state after explicit set: ret={ret}, state={state}, pending={pending}", flush=True)
        
        # Final element state check
        print(f"[srt-debug] srtserversrc final state: {srt_src.get_state(0)[1]}", flush=True)
        print(f"[srt-debug] decodebin final state: {decode.get_state(0)[1]}", flush=True)
        
        print("[srt] ✓ SRT elements added, listening on port 1937", flush=True)
    
    def _on_srt_pad_added(self, decodebin, pad):
        """Handle dynamic pad creation from decodebin when SRT connects."""
        caps = pad.get_current_caps()
        name = caps.to_string() if caps else ""
        print(f"[srt] Decodebin pad added: {name}", flush=True)
        
        if not self.srt_ever_connected:
            self.srt_ever_connected = True
            self.srt_connected = True
            print("[srt] ✓ First SRT connection successful", flush=True)
        
        if name.startswith("video/"):
            sinkpad = self.srt_elements['video_queue'].get_static_pad("sink")
            if not sinkpad.is_linked():
                ret = pad.link(sinkpad)
                print(f"[srt] Video pad link result: {ret}", flush=True)
                
                compositor = self.output_elements['compositor']
                self.srt_compositor_pad = compositor.request_pad_simple("sink_%u")
                self.srt_compositor_pad.set_property("alpha", 0.0)
                videorate = self.srt_elements['videorate']
                vrate_src = videorate.get_static_pad("src")
                vrate_src.link(self.srt_compositor_pad)
                print(f"[srt] ✓ Video linked to compositor pad (alpha=0.0)", flush=True)
                
        elif name.startswith("audio/"):
            sinkpad = self.srt_elements['audio_queue'].get_static_pad("sink")
            if not sinkpad.is_linked():
                ret = pad.link(sinkpad)
                print(f"[srt] Audio pad link result: {ret}", flush=True)
                
                audiomixer = self.output_elements['audiomixer']
                self.srt_mixer_pad = audiomixer.request_pad_simple("sink_%u")
                vol_src = self.cam_vol.get_static_pad("src")
                vol_src.link(self.srt_mixer_pad)
                print("[srt] ✓ Audio linked to audiomixer", flush=True)
    
    def _on_srt_video_probe(self, pad, info):
        """Track SRT buffer flow and trigger buffering/fade-in."""
        self.last_srt_buf_time = time.time()
        
        # Update bitrate from buffer metadata if available
        if self.srt_elements:
            try:
                srt_src = self.srt_elements.get('srt_src')
                if srt_src:
                    stats = srt_src.get_property("stats")
                    if stats and isinstance(stats, str):
                        # Parse stats string for bitrate information
                        import re
                        match = re.search(r'bitrate[=:]\s*(\d+)', stats)
                        if match:
                            bitrate_bps = int(match.group(1))
                            self.srt_bitrate_kbps = int(bitrate_bps / 1024)
            except:
                pass
        
        if self.state == STATE_SRT_TRANSITIONING:
            buffer = info.get_buffer()
            alpha_val = self.srt_compositor_pad.get_property("alpha") if self.srt_compositor_pad else 0.0
            print(f"[probe] SRT_TRANSITIONING: buffer flowing, pad_alpha={alpha_val:.2f}, pts={buffer.pts/Gst.SECOND:.3f}s", flush=True)
        
        # Start buffering on first buffer
        if self.state in (STATE_FALLBACK_ONLY, STATE_VIDEO_CONNECTED):
            if self.srt_first_buffer_time is None:
                self.srt_first_buffer_time = time.time()
                self.state = STATE_SRT_BUFFERING
                print(f"[buffer] SRT buffering started, waiting {BUFFER_DELAY_MS}ms before fade-in...", flush=True)
                # Schedule fade-in after buffer delay
                if not self.srt_buffer_scheduled:
                    self.srt_buffer_scheduled = True
                    GLib.timeout_add(BUFFER_DELAY_MS, self._start_srt_fade_after_buffer)
        
        return Gst.PadProbeReturn.OK
    
    def remove_srt_elements(self):
        """Remove SRT elements from the pipeline with robust error handling."""
        if not self.srt_elements:
            print("[srt] No SRT elements to remove", flush=True)
            return
        
        print("[srt] Removing SRT elements from pipeline...", flush=True)
        
        # Track removal progress
        failed_elements = []
        
        for elem_name, elem in self.srt_elements.items():
            try:
                print(f"[srt] Attempting to remove element: {elem_name}", flush=True)
                
                # Set element to NULL state with timeout to prevent hanging
                elem.set_state(Gst.State.NULL)
                
                # Wait for state change with timeout (prevents infinite blocking)
                ret, state, pending = elem.get_state(ELEMENT_REMOVAL_TIMEOUT_SEC * Gst.SECOND)
                
                if ret == Gst.StateChangeReturn.FAILURE:
                    print(f"[srt] ⚠ State transition to NULL failed for {elem_name}, attempting removal anyway", flush=True)
                elif ret == Gst.StateChangeReturn.ASYNC:
                    print(f"[srt] ⚠ State transition timeout for {elem_name} (timeout={ELEMENT_REMOVAL_TIMEOUT_SEC}s), forcing removal", flush=True)
                else:
                    print(f"[srt] ✓ State transition successful for {elem_name}", flush=True)
                
                # Attempt to remove from pipeline
                try:
                    self.pipeline.remove(elem)
                    print(f"[srt] ✓ Element {elem_name} removed from pipeline", flush=True)
                except Exception as remove_err:
                    print(f"[srt] ⚠ Failed to remove {elem_name} from pipeline: {remove_err}", flush=True)
                    failed_elements.append(elem_name)
                    
            except Exception as e:
                print(f"[srt] ⚠ Exception while removing {elem_name}: {e}", flush=True)
                failed_elements.append(elem_name)
                # Continue with next element instead of failing completely
                continue
        
        # Clear references regardless of individual failures
        self.srt_elements = None
        self.srt_compositor_pad = None
        self.srt_mixer_pad = None
        self.cam_vol = None
        self.srt_connected = False
        self.srt_bitrate_kbps = 0
        
        if failed_elements:
            print(f"[srt] ⚠ SRT element removal completed with errors for: {', '.join(failed_elements)}", flush=True)
        else:
            print("[srt] ✓ SRT elements removed successfully", flush=True)
    
    def restart_srt_elements(self):
        """Restart SRT elements after a timeout."""
        print("[srt] Restarting SRT elements...", flush=True)
        self.restart_scheduled = False
        
        self.remove_srt_elements()
        
        # Reset buffer tracking
        self.srt_first_buffer_time = None
        self.srt_buffer_scheduled = False
        
        self.add_srt_elements()
        
        print("[srt] ✓ SRT restart complete, waiting for connection...", flush=True)
        return False
    
    def _start_video_fade_after_buffer(self):
        """Callback to start video fade-in after buffer delay."""
        self.video_buffer_scheduled = False
        
        if self.state == STATE_VIDEO_BUFFERING:
            elapsed = time.time() - (self.video_first_buffer_time or 0)
            print(f"[buffer] Video buffering complete ({elapsed*1000:.0f}ms), starting fade-in", flush=True)
            self.start_fade_in("video")
        else:
            print(f"[buffer] Video buffer timeout but state changed to {self.state}, skipping fade", flush=True)
        
        return False
    
    def _start_srt_fade_after_buffer(self):
        """Callback to start SRT fade-in after buffer delay."""
        self.srt_buffer_scheduled = False
        
        # Determine the previous state to handle crossfade
        if self.state == STATE_SRT_BUFFERING:
            elapsed = time.time() - (self.srt_first_buffer_time or 0)
            print(f"[buffer] SRT buffering complete ({elapsed*1000:.0f}ms), starting fade-in", flush=True)
            self.start_fade_in("srt")
        else:
            print(f"[buffer] SRT buffer timeout but state changed to {self.state}, skipping fade", flush=True)
        
        return False
    
    def start_fade_in(self, source_type="srt"):
        """Fade in video/audio with crossfade support."""
        if source_type == "video":
            if self.state in (STATE_VIDEO_TRANSITIONING, STATE_VIDEO_CONNECTED,
                            STATE_SRT_TRANSITIONING, STATE_SRT_CONNECTED, STATE_SRT_BUFFERING):
                return
            print("[fade] Starting fade-in (video)", flush=True)
            self.state = STATE_VIDEO_TRANSITIONING
            self._crossfade(None, "video", 1000, STATE_VIDEO_CONNECTED)
        else:  # srt
            # Check privacy mode first
            if self.privacy_enabled:
                print("[fade] SRT fade-in BLOCKED by privacy mode", flush=True)
                return
            
            if self.state in (STATE_SRT_TRANSITIONING, STATE_SRT_CONNECTED):
                return
            
            # Determine if we need to crossfade with video
            if self.video_elements and self.video_ever_connected and self.video_compositor_pad:
                current_video_alpha = self.video_compositor_pad.get_property("alpha")
                if current_video_alpha > 0.5:
                    print("[fade] Starting crossfade (video → SRT)", flush=True)
                    self.state = STATE_SRT_TRANSITIONING
                    self._crossfade("video", "srt", 1000, STATE_SRT_CONNECTED)
                    return
            
            print("[fade] Starting fade-in (SRT)", flush=True)
            self.state = STATE_SRT_TRANSITIONING
            self._crossfade(None, "srt", 1000, STATE_SRT_CONNECTED)
    
    def start_fade_out(self, source_type="srt"):
        """Fade out video/audio with crossfade support."""
        if source_type == "video":
            if self.state in (STATE_VIDEO_TRANSITIONING, STATE_VIDEO_BUFFERING, STATE_FALLBACK_ONLY):
                print(f"[fade] start_fade_out(video) blocked (already in state: {self.state})", flush=True)
                return
            print("[fade] Starting fade-out (video)", flush=True)
            self.state = STATE_VIDEO_TRANSITIONING
            self._crossfade("video", None, 1000, STATE_FALLBACK_ONLY)
        else:  # srt
            # Diagnostic logging for SRT fade-out
            print(f"[fade-debug] start_fade_out(srt) called, current_state={self.state}", flush=True)
            
            # CRITICAL FIX: Check if already transitioning FIRST to prevent overlapping crossfades
            if self.state in (STATE_SRT_TRANSITIONING, STATE_SRT_BUFFERING, STATE_FALLBACK_ONLY):
                print(f"[fade] start_fade_out(srt) blocked (already in state: {self.state})", flush=True)
                return
            
            # Determine next state and crossfade target
            if self.video_elements and self.video_ever_connected and self.video_compositor_pad:
                current_video_alpha = self.video_compositor_pad.get_property("alpha")
                current_srt_alpha = self.srt_compositor_pad.get_property("alpha") if self.srt_compositor_pad else 0.0
                print(f"[fade-debug] Video present: video_alpha={current_video_alpha:.2f}, srt_alpha={current_srt_alpha:.2f}", flush=True)
                
                if current_video_alpha < 0.5:
                    # Video is faded out, crossfade to it
                    print(f"[fade] Starting crossfade (SRT → video) - video_alpha < 0.5", flush=True)
                    self.state = STATE_SRT_TRANSITIONING
                    self._crossfade("srt", "video", 1000, STATE_VIDEO_CONNECTED)
                    return
                else:
                    print(f"[fade-debug] Video already visible (alpha={current_video_alpha:.2f}), fading out SRT only", flush=True)
            else:
                print(f"[fade-debug] No video fallback available, fading to black", flush=True)
            
            print("[fade] Starting fade-out (SRT)", flush=True)
            self.state = STATE_SRT_TRANSITIONING
            next_state = STATE_VIDEO_CONNECTED if (self.video_elements and self.video_ever_connected) else STATE_FALLBACK_ONLY
            print(f"[fade-debug] Calling _crossfade('srt', None, 1000, {next_state})", flush=True)
            self._crossfade("srt", None, 1000, next_state)
    
    def _crossfade(self, fade_out_layer, fade_in_layer, duration_ms, next_state):
        """
        Crossfade between two layers.
        
        Args:
            fade_out_layer: Layer to fade out ("video", "srt", or None for black)
            fade_in_layer: Layer to fade in ("video", "srt", or None for black)
            duration_ms: Duration of crossfade in milliseconds
            next_state: State to transition to after crossfade completes
        """
        # Capture initial alpha values for diagnostic
        initial_video_alpha = self.video_compositor_pad.get_property("alpha") if self.video_compositor_pad else 0.0
        initial_srt_alpha = self.srt_compositor_pad.get_property("alpha") if self.srt_compositor_pad else 0.0
        print(f"[crossfade-debug] Starting crossfade: out={fade_out_layer}, in={fade_in_layer}, next_state={next_state}", flush=True)
        print(f"[crossfade-debug] Initial alphas: video={initial_video_alpha:.2f}, srt={initial_srt_alpha:.2f}", flush=True)
        
        steps = 10
        step_ms = max(1, duration_ms // steps)
        step = {"i": 0}
        
        def step_cb():
            i = step["i"]
            t = i / float(steps)
            
            # Calculate alpha and volume for fade out layer
            alpha_out = 1.0 - t
            vol_out = 1.0 - t
            
            # Calculate alpha and volume for fade in layer
            alpha_in = t
            vol_in = t
            
            # Apply fade out
            if fade_out_layer == "video":
                if self.video_compositor_pad:
                    self.video_compositor_pad.set_property("alpha", alpha_out)
                    print(f"[crossfade-debug] Setting video alpha to {alpha_out:.2f}", flush=True)
                if self.video_vol:
                    self.video_vol.set_property("volume", vol_out)
            elif fade_out_layer == "srt":
                if self.srt_compositor_pad:
                    self.srt_compositor_pad.set_property("alpha", alpha_out)
                    print(f"[crossfade-debug] Setting srt alpha to {alpha_out:.2f}", flush=True)
                if self.cam_vol:
                    self.cam_vol.set_property("volume", vol_out)
            
            # Apply fade in
            if fade_in_layer == "video":
                if self.video_compositor_pad:
                    self.video_compositor_pad.set_property("alpha", alpha_in)
                    print(f"[crossfade-debug] Setting video alpha to {alpha_in:.2f}", flush=True)
                if self.video_vol:
                    self.video_vol.set_property("volume", vol_in)
            elif fade_in_layer == "srt":
                if self.srt_compositor_pad:
                    self.srt_compositor_pad.set_property("alpha", alpha_in)
                    print(f"[crossfade-debug] Setting srt alpha to {alpha_in:.2f}", flush=True)
                if self.cam_vol:
                    self.cam_vol.set_property("volume", vol_in)
            
            # Log progress
            out_str = f"{fade_out_layer}↓{alpha_out:.2f}" if fade_out_layer else ""
            in_str = f"{fade_in_layer}↑{alpha_in:.2f}" if fade_in_layer else ""
            separator = " | " if out_str and in_str else ""
            print(f"[crossfade] Step {i}/{steps}: {out_str}{separator}{in_str}, state={self.state}", flush=True)
            
            step["i"] += 1
            if step["i"] > steps:
                # Set final values
                if fade_out_layer == "video":
                    if self.video_compositor_pad:
                        self.video_compositor_pad.set_property("alpha", 0.0)
                    if self.video_vol:
                        self.video_vol.set_property("volume", 0.0)
                elif fade_out_layer == "srt":
                    if self.srt_compositor_pad:
                        self.srt_compositor_pad.set_property("alpha", 0.0)
                    if self.cam_vol:
                        self.cam_vol.set_property("volume", 0.0)
                
                if fade_in_layer == "video":
                    if self.video_compositor_pad:
                        self.video_compositor_pad.set_property("alpha", 1.0)
                    if self.video_vol:
                        self.video_vol.set_property("volume", 1.0)
                elif fade_in_layer == "srt":
                    if self.srt_compositor_pad:
                        self.srt_compositor_pad.set_property("alpha", 1.0)
                    if self.cam_vol:
                        self.cam_vol.set_property("volume", 1.0)
                
                self.state = next_state
                print(f"[crossfade] Crossfade finished, new state: {self.state}", flush=True)
                
                # Schedule restarts if needed
                if fade_out_layer == "video" and next_state == STATE_FALLBACK_ONLY and self.video_ever_connected:
                    if not self.video_restart_scheduled:
                        self.video_restart_scheduled = True
                        GLib.timeout_add_seconds(2, self.restart_video_elements)
                elif fade_out_layer == "srt" and next_state in (STATE_FALLBACK_ONLY, STATE_VIDEO_CONNECTED) and self.srt_ever_connected:
                    if not self.restart_scheduled:
                        self.restart_scheduled = True
                        GLib.timeout_add_seconds(2, self.restart_srt_elements)
                
                return False
            return True
        
        GLib.timeout_add(step_ms, step_cb)
    
    def watchdog_cb(self):
        """Check if SRT has stopped sending data."""
        now = time.time()
        delta = now - self.last_srt_buf_time
        
        if self.state in (STATE_SRT_CONNECTED, STATE_SRT_TRANSITIONING, STATE_SRT_BUFFERING) and delta > 0.2:
            if self.state == STATE_SRT_BUFFERING:
                print(f"[watchdog] No SRT data during buffering for {delta:.1f}s, cancelling buffer", flush=True)
                self.state = STATE_VIDEO_CONNECTED if (self.video_elements and self.video_ever_connected) else STATE_FALLBACK_ONLY
                self.srt_first_buffer_time = None
                self.srt_buffer_scheduled = False
            else:
                print(f"[watchdog] No SRT data for {delta:.1f}s, state={self.state}, triggering fade-out", flush=True)
                self.start_fade_out("srt")
        
        return True
    
    def on_message(self, bus, msg):
        """Handle pipeline bus messages."""
        t = msg.type
        src = msg.src.get_name() if msg.src else "unknown"
        
        # Log all SRT-related messages for debugging
        if "srt" in src.lower() or "decode" in src.lower():
            print(f"[bus-debug] {str(t)} from {src}", flush=True)
        
        if t == Gst.MessageType.ERROR:
            err, debug = msg.parse_error()
            err_domain = err.domain
            err_message = str(err.message)
            
            if "TCP" in err_message and self.video_elements:
                print(f"[bus] INFO: TCP video client disconnected - will restart after fade", flush=True)
            elif "gst-resource-error-quark" in err_domain and "SRT socket" in err_message:
                print(f"[bus] INFO: SRT client disconnected - will restart after fade", flush=True)
            else:
                print(f"[bus] ERROR from {src}: {err} | {debug}", flush=True)
                
        elif t == Gst.MessageType.EOS:
            print(f"[bus] EOS received from {src}, triggering fade-out", flush=True)
            if self.state in (STATE_SRT_CONNECTED, STATE_SRT_TRANSITIONING):
                self.start_fade_out("srt")
            elif self.state in (STATE_VIDEO_CONNECTED, STATE_VIDEO_TRANSITIONING):
                self.start_fade_out("video")
            
        elif t == Gst.MessageType.WARNING:
            warn, debug = msg.parse_warning()
            warn_message = str(warn.message)
            
            if "TCP" in warn_message or "SRT" in warn_message:
                print(f"[bus] INFO: Connection warning from {src} - {warn_message}", flush=True)
            else:
                print(f"[bus] WARNING from {src}: {warn} | {debug}", flush=True)
        
        elif t == Gst.MessageType.STATE_CHANGED:
            if "srt" in src.lower():
                old, new, pending = msg.parse_state_changed()
                print(f"[bus-debug] {src} state: {str(old)} -> {str(new)} (pending: {str(pending)})", flush=True)
        
        elif t == Gst.MessageType.STREAM_START:
            if "srt" in src.lower() or "decode" in src.lower():
                print(f"[bus-debug] STREAM_START from {src}", flush=True)
        
        elif t == Gst.MessageType.NEW_CLOCK:
            if "srt" in src.lower() or "decode" in src.lower():
                print(f"[bus-debug] NEW_CLOCK from {src}", flush=True)
        
        elif t == Gst.MessageType.ASYNC_DONE:
            if "srt" in src.lower():
                print(f"[bus-debug] ASYNC_DONE from {src}", flush=True)
    
    def start_pipeline(self):
        """Build and start the complete pipeline."""
        print(f"[compositor] Starting v{__version__}...", flush=True)
        
        # Build all stages
        self._build_fallback_sources()
        self._build_output_stage()
        self._link_fallback_to_output()
        
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_message)
        
        # Start pipeline
        print("[compositor] Setting pipeline to PLAYING...", flush=True)
        ret = self.pipeline.set_state(Gst.State.PLAYING)
        print(f"[compositor] set_state(PLAYING) returned: {ret.value_nick}", flush=True)
        
        ret, state, pending = self.pipeline.get_state(5 * Gst.SECOND)
        print(f"[compositor] get_state result: {ret.value_nick}, state={state.value_nick}, pending={pending.value_nick}", flush=True)
        
        if ret == Gst.StateChangeReturn.SUCCESS or ret == Gst.StateChangeReturn.ASYNC:
            print("[compositor] ✓ Pipeline PLAYING with fallback output", flush=True)
        else:
            print(f"[compositor] ⚠ Pipeline state change issue: {ret.value_nick}", flush=True)
            
        print("[compositor] TCP output: tcp://0.0.0.0:5000", flush=True)
        
        # Add video fallback if configured
        if FALLBACK_SOURCE == "video":
            print(f"[compositor] FALLBACK_SOURCE=video, adding TCP video server on port {VIDEO_TCP_PORT}", flush=True)
            self.add_video_elements()
            GLib.timeout_add(200, self.video_watchdog_cb)
        
        # Add SRT elements
        self.add_srt_elements()
        
        # Start SRT watchdog
        GLib.timeout_add(200, self.watchdog_cb)
        
        if FALLBACK_SOURCE == "video":
            print(f"[compositor] ✓ Ready - streaming black screen, waiting for video TCP on port {VIDEO_TCP_PORT} and SRT on port 1937", flush=True)
        else:
            print("[compositor] ✓ Ready - streaming black screen, waiting for SRT on port 1937", flush=True)
    
    def run(self):
        """Run the main loop."""
        self.loop = GLib.MainLoop()
        
        # Signal handler for graceful shutdown
        def signal_handler(signum, frame):
            sig_name = signal.Signals(signum).name
            print(f"\n[compositor] Received {sig_name}, shutting down gracefully...", flush=True)
            if self.loop:
                self.loop.quit()
        
        # Register signal handlers for SIGTERM (Docker stop) and SIGINT (Ctrl+C)
        signal.signal(signal.SIGTERM, signal_handler)
        signal.signal(signal.SIGINT, signal_handler)
        print("[compositor] Signal handlers registered (SIGTERM, SIGINT)", flush=True)
        
        try:
            self.loop.run()
        except KeyboardInterrupt:
            print("\n[compositor] Shutting down...", flush=True)
        finally:
            print("[compositor] Cleaning up pipeline...", flush=True)
            self.pipeline.set_state(Gst.State.NULL)
            print("[compositor] ✓ Shutdown complete", flush=True)


class CompositorHTTPHandler(BaseHTTPRequestHandler):
    """HTTP request handler for compositor API."""
    
    compositor_manager = None
    
    def log_message(self, format, *args):
        """Override to silence HTTP request logging."""
        pass
    
    def send_json_response(self, data, status=200):
        """Send JSON response."""
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())
    
    def do_OPTIONS(self):
        """Handle CORS preflight."""
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()
    
    def do_GET(self):
        """Handle GET requests."""
        parsed = urlparse(self.path)
        path = parsed.path
        
        if path == '/health':
            # Get SRT connection status, bitrate, and current scene
            srt_stats = self.compositor_manager.get_srt_stats()
            scene = self.compositor_manager.get_current_scene()
            response = {
                'status': 'ok',
                'scene': scene,
                'srt_connected': srt_stats['connected'],
                'srt_bitrate_kbps': srt_stats['bitrate_kbps'],
                'privacy_enabled': self.compositor_manager.privacy_enabled
            }
            self.send_json_response(response)
        
        elif path == '/privacy':
            enabled = self.compositor_manager.privacy_enabled
            self.send_json_response({'enabled': enabled})
        
        else:
            self.send_json_response({'error': 'Not found'}, 404)
    
    def do_POST(self):
        """Handle POST requests."""
        parsed = urlparse(self.path)
        path = parsed.path
        
        if path == '/privacy':
            content_length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(content_length)
            
            try:
                data = json.loads(body.decode()) if body else {}
                enabled = data.get('enabled', False)
                
                self.compositor_manager.set_privacy_mode(enabled)
                self.send_json_response({
                    'success': True,
                    'enabled': enabled,
                    'message': f"Privacy mode {'enabled' if enabled else 'disabled'}"
                })
            except Exception as e:
                print(f"[http] Error handling privacy request: {e}", flush=True)
                self.send_json_response({'error': str(e)}, 400)
        
        else:
            self.send_json_response({'error': 'Not found'}, 404)


def run_http_server(compositor_manager):
    """Run HTTP API server in background thread."""
    CompositorHTTPHandler.compositor_manager = compositor_manager
    server = HTTPServer(('0.0.0.0', HTTP_API_PORT), CompositorHTTPHandler)
    print(f"[http] HTTP API server listening on port {HTTP_API_PORT}", flush=True)
    print("[http] Endpoints:", flush=True)
    print("[http]   GET  /health  - Health check with scene info", flush=True)
    print("[http]   GET  /privacy - Get privacy mode status", flush=True)
    print("[http]   POST /privacy - Set privacy mode (JSON: {\"enabled\": true/false})", flush=True)
    server.serve_forever()


def main():
    compositor = CompositorManager()
    
    # Start HTTP API server in background thread
    http_thread = threading.Thread(target=run_http_server, args=(compositor,), daemon=True)
    http_thread.start()
    
    compositor.start_pipeline()
    compositor.run()


if __name__ == "__main__":
    main()