#!/usr/bin/env python3
"""
GStreamer Compositor with Hybrid Architecture
Version: 2.0.0

Architecture:
- Fallback pipeline (always running): black screen + silence
- SRT pipeline (dynamic): added on startup, can be removed/re-added
- Shared output (always running): compositor + audiomixer + encoders + TCP sink

The pipeline starts immediately with fallback output, and SRT feed composites
over the fallback when available.
"""
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gst, GLib
import time

Gst.init(None)

__version__ = "2.0.0"

# State constants
STATE_FALLBACK_ONLY = "FALLBACK_ONLY"
STATE_SRT_TRANSITIONING = "SRT_TRANSITIONING"
STATE_SRT_CONNECTED = "SRT_CONNECTED"


class CompositorManager:
    """Manages a GStreamer compositor with hybrid architecture."""
    
    def __init__(self):
        self.pipeline = Gst.Pipeline.new("hybrid_compositor")
        
        # Element storage
        self.fallback_elements = {}
        self.output_elements = {}
        self.srt_elements = None
        
        # State tracking
        self.state = STATE_FALLBACK_ONLY
        self.last_srt_buf_time = time.time()
        self.srt_ever_connected = False
        self.restart_scheduled = False
        
        # Fade control references
        self.cam_alpha = None
        self.cam_vol = None
        
    def _build_fallback_sources(self):
        """Create always-running fallback sources (black + silence)."""
        print("[build] Creating fallback sources (black + silence)...", flush=True)
        
        # Black video source
        black_src = Gst.ElementFactory.make("videotestsrc", "black_src")
        black_src.set_property("pattern", 2)  # black
        black_src.set_property("is-live", True)
        black_src.set_property("do-timestamp", True)  # Generate timestamps
        
        black_capsfilter = Gst.ElementFactory.make("capsfilter", "black_caps")
        black_caps = Gst.Caps.from_string("video/x-raw,width=1920,height=1080,framerate=30/1")
        black_capsfilter.set_property("caps", black_caps)
        
        black_vconv = Gst.ElementFactory.make("videoconvert", "black_vconv")
        
        # Silent audio source
        silence_src = Gst.ElementFactory.make("audiotestsrc", "silence_src")
        silence_src.set_property("wave", 4)  # silence
        silence_src.set_property("is-live", True)
        silence_src.set_property("do-timestamp", True)  # Generate timestamps
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
        
        # DIAGNOSTIC: Log encoder properties
        print(f"[build] x264enc config: tune=zerolatency, preset=superfast, bitrate=2500, key-int-max=60", flush=True)
        
        # Queue after video encoder (helps muxer synchronize)
        video_mux_queue = Gst.ElementFactory.make("queue", "video_mux_q")
        
        # Audio encoding
        aacenc = Gst.ElementFactory.make("avenc_aac", "aac")
        aacenc.set_property("bitrate", 128000)
        
        # Queue after audio encoder (helps muxer synchronize)
        audio_mux_queue = Gst.ElementFactory.make("queue", "audio_mux_q")
        
        # Muxer and output (back to mpegtsmux)
        mpegtsmux = Gst.ElementFactory.make("mpegtsmux", "mux")
        mpegtsmux.set_property("alignment", 7)
        
        # DIAGNOSTIC: Log muxer state and add state change tracking
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
        
        # Link video encoding chain with queue
        compositor.link(vconv_out)
        vconv_out.link(x264enc)
        x264enc.link(video_mux_queue)
        
        # Link TCP sink to muxer
        mpegtsmux.link(tcp_sink)
        
        # NOTE: Encoders will be linked to muxer AFTER sources are connected
        # This prevents muxer from blocking while waiting for streams
        
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
        
        # Link audiomixer to aacenc with queue
        aacenc = self.output_elements['aacenc']
        audio_mux_queue = self.output_elements['audio_mux_queue']
        mixer_link_result = audiomixer.link(aacenc)
        print(f"[build] Audiomixer → aacenc link: {mixer_link_result}", flush=True)
        aacenc.link(audio_mux_queue)
        
        # NOW link queues to muxer (after sources are connected)
        mpegtsmux = self.output_elements['mpegtsmux']
        video_mux_queue = self.output_elements['video_mux_queue']
        
        video_to_mux = video_mux_queue.link(mpegtsmux)
        audio_to_mux = audio_mux_queue.link(mpegtsmux)
        print(f"[build] video_queue → mux link: {video_to_mux}", flush=True)
        print(f"[build] audio_queue → mux link: {audio_to_mux}", flush=True)
        
        # Add buffer probes to track data flow WITH TIMESTAMP INFO
        def buffer_probe_detailed(pad, info, name):
            """Detailed buffer probe showing timestamps and buffer info."""
            buffer = info.get_buffer()
            pts = buffer.pts
            dts = buffer.dts
            duration = buffer.duration
            
            # Convert to human-readable format
            pts_str = f"{pts/Gst.SECOND:.3f}s" if pts != Gst.CLOCK_TIME_NONE else "NONE"
            dts_str = f"{dts/Gst.SECOND:.3f}s" if dts != Gst.CLOCK_TIME_NONE else "NONE"
            dur_str = f"{duration/Gst.MSECOND:.1f}ms" if duration != Gst.CLOCK_TIME_NONE else "NONE"
            
            print(f"[probe] {name:20s} | PTS={pts_str:12s} DTS={dts_str:12s} DUR={dur_str:10s} size={buffer.get_size()}", flush=True)
            return Gst.PadProbeReturn.OK
        
        # Get elements for probing
        x264enc = self.output_elements['x264enc']
        
        # Probe fallback sources to verify they have timestamps
        black_src = self.fallback_elements['black_src']
        black_src_pad = black_src.get_static_pad("src")
        black_src_pad.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "black_src(raw)"))
        
        silence_src = self.fallback_elements['silence_src']
        silence_src_pad = silence_src.get_static_pad("src")
        silence_src_pad.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "silence_src(raw)"))
        
        # Probe compositor output (raw video)
        comp_src = compositor.get_static_pad("src")
        comp_src.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "compositor→x264"))
        
        # Probe x264 INPUT to see if timestamps arrive
        x264_sink = x264enc.get_static_pad("sink")
        x264_sink.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "→x264(input)"))
        
        # Probe x264 output (encoded video)
        x264_src = x264enc.get_static_pad("src")
        x264_src.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "x264→queue"))
        
        # Probe video queue output before muxer
        video_mux_queue_src = video_mux_queue.get_static_pad("src")
        video_mux_queue_src.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "video_q→mux"))
        
        # Probe audiomixer output (raw audio)
        mixer_src = audiomixer.get_static_pad("src")
        mixer_src.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "audiomixer→aac"))
        
        # Probe aac INPUT to see if timestamps arrive
        aac_sink = aacenc.get_static_pad("sink")
        aac_sink.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "→aac(input)"))
        
        # Probe aac output (encoded audio)
        aac_src = aacenc.get_static_pad("src")
        aac_src.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "aac→queue"))
        
        # Probe audio queue output before muxer
        audio_mux_queue_src = audio_mux_queue.get_static_pad("src")
        audio_mux_queue_src.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "audio_q→mux"))
        
        # Probe muxer inputs
        print("[probe] Note: muxer input pads will be probed once linked", flush=True)
        
        # Probe muxer output
        mux_src = mpegtsmux.get_static_pad("src")
        mux_src.add_probe(Gst.PadProbeType.BUFFER, lambda p, i: buffer_probe_detailed(p, i, "mux→tcp"))
        
        print("[build] ✓ Fallback linked to output", flush=True)
    
    def add_srt_elements(self):
        """Add SRT source elements to the running pipeline."""
        if self.srt_elements:
            print("[srt] SRT elements already added", flush=True)
            return
        
        print("[srt] Adding SRT elements to pipeline...", flush=True)
        
        # Create SRT source chain
        srt_src = Gst.ElementFactory.make("srtserversrc", "srt_src")
        srt_src.set_property("uri", "srt://:1937?mode=listener")
        srt_src.set_property("latency", 2000)
        
        decode = Gst.ElementFactory.make("decodebin", "decode")
        
        # Video chain
        video_queue = Gst.ElementFactory.make("queue", "video_q")
        videoconvert = Gst.ElementFactory.make("videoconvert", "vconv")
        videoscale = Gst.ElementFactory.make("videoscale", "vscale")
        videorate = Gst.ElementFactory.make("videorate", "vrate")
        videorate.set_property("drop-only", True)
        
        self.cam_alpha = Gst.ElementFactory.make("alpha", "cam_alpha")
        self.cam_alpha.set_property("alpha", 0.0)  # start invisible
        
        # Audio chain
        audio_queue = Gst.ElementFactory.make("queue", "audio_q")
        audio_queue.set_property("max-size-time", 0)
        audio_queue.set_property("max-size-buffers", 0)
        audioconvert = Gst.ElementFactory.make("audioconvert", "aconv")
        audioresample = Gst.ElementFactory.make("audioresample", "ares")
        
        self.cam_vol = Gst.ElementFactory.make("volume", "cam_vol")
        self.cam_vol.set_property("volume", 0.0)  # start muted
        
        self.srt_elements = {
            'srt_src': srt_src,
            'decode': decode,
            'video_queue': video_queue,
            'videoconvert': videoconvert,
            'videoscale': videoscale,
            'videorate': videorate,
            'cam_alpha': self.cam_alpha,
            'audio_queue': audio_queue,
            'audioconvert': audioconvert,
            'audioresample': audioresample,
            'cam_vol': self.cam_vol,
        }
        
        # Add to pipeline
        for elem in self.srt_elements.values():
            self.pipeline.add(elem)
        
        # Link SRT source to decodebin
        srt_src.link(decode)
        
        # Link video chain
        video_queue.link(videoconvert)
        videoconvert.link(videoscale)
        videoscale.link(videorate)
        videorate.link(self.cam_alpha)
        
        # Link audio chain
        audio_queue.link(audioconvert)
        audioconvert.link(audioresample)
        audioresample.link(self.cam_vol)
        
        # Connect pad-added signal for dynamic linking
        decode.connect("pad-added", self._on_srt_pad_added)
        
        # Add buffer probe to track SRT activity
        video_queue_src = video_queue.get_static_pad("src")
        video_queue_src.add_probe(Gst.PadProbeType.BUFFER, self._on_video_probe)
        
        # Sync state with parent
        for elem in self.srt_elements.values():
            elem.sync_state_with_parent()
        
        print("[srt] ✓ SRT elements added, listening on port 1937", flush=True)
    
    def _on_srt_pad_added(self, decodebin, pad):
        """Handle dynamic pad creation from decodebin when SRT connects."""
        caps = pad.get_current_caps()
        name = caps.to_string() if caps else ""
        print(f"[srt] Decodebin pad added: {name}", flush=True)
        
        # Mark that SRT has connected successfully
        if not self.srt_ever_connected:
            self.srt_ever_connected = True
            print("[srt] ✓ First SRT connection successful", flush=True)
        
        if name.startswith("video/"):
            # Link decode → video_queue
            sinkpad = self.srt_elements['video_queue'].get_static_pad("sink")
            if not sinkpad.is_linked():
                ret = pad.link(sinkpad)
                print(f"[srt] Video pad link result: {ret}", flush=True)
                
                # Link cam_alpha to compositor
                compositor = self.output_elements['compositor']
                cam_pad = compositor.request_pad_simple("sink_%u")
                alpha_src = self.cam_alpha.get_static_pad("src")
                alpha_src.link(cam_pad)
                print("[srt] ✓ Video linked to compositor", flush=True)
                
        elif name.startswith("audio/"):
            # Link decode → audio_queue
            sinkpad = self.srt_elements['audio_queue'].get_static_pad("sink")
            if not sinkpad.is_linked():
                ret = pad.link(sinkpad)
                print(f"[srt] Audio pad link result: {ret}", flush=True)
                
                # Link cam_vol to audiomixer
                audiomixer = self.output_elements['audiomixer']
                mixer_pad = audiomixer.request_pad_simple("sink_%u")
                vol_src = self.cam_vol.get_static_pad("src")
                vol_src.link(mixer_pad)
                print("[srt] ✓ Audio linked to audiomixer", flush=True)
    
    def _on_video_probe(self, pad, info):
        """Track SRT buffer flow and trigger fade-in when reconnecting."""
        self.last_srt_buf_time = time.time()
        
        # If we were in FALLBACK_ONLY, trigger fade-in
        if self.state == STATE_FALLBACK_ONLY:
            self.start_fade_in()
        
        return Gst.PadProbeReturn.OK
    
    def remove_srt_elements(self):
        """Remove SRT elements from the pipeline (after fade-out)."""
        if not self.srt_elements:
            print("[srt] No SRT elements to remove", flush=True)
            return
        
        print("[srt] Removing SRT elements from pipeline...", flush=True)
        
        # Set elements to NULL and remove from pipeline
        for elem in self.srt_elements.values():
            elem.set_state(Gst.State.NULL)
            self.pipeline.remove(elem)
        
        self.srt_elements = None
        self.cam_alpha = None
        self.cam_vol = None
        
        print("[srt] ✓ SRT elements removed", flush=True)
    
    def restart_srt_elements(self):
        """Restart SRT elements after a timeout (allows reconnection)."""
        print("[srt] Restarting SRT elements...", flush=True)
        self.restart_scheduled = False
        
        # Remove old elements
        self.remove_srt_elements()
        
        # Add fresh elements
        self.add_srt_elements()
        
        print("[srt] ✓ SRT restart complete, waiting for connection...", flush=True)
        return False  # Don't repeat timeout
    
    def start_fade_in(self):
        """Fade in SRT video/audio (0.0 → 1.0 over 1 second)."""
        if self.state in (STATE_SRT_TRANSITIONING, STATE_SRT_CONNECTED):
            return
        print("[fade] Starting fade-in", flush=True)
        self.state = STATE_SRT_TRANSITIONING
        self._fade(0.0, 1.0, 0.0, 1.0, 1000, STATE_SRT_CONNECTED)
    
    def start_fade_out(self):
        """Fade out SRT video/audio (1.0 → 0.0 over 1 second)."""
        if self.state in (STATE_SRT_TRANSITIONING, STATE_FALLBACK_ONLY):
            return
        print("[fade] Starting fade-out", flush=True)
        self.state = STATE_SRT_TRANSITIONING
        self._fade(1.0, 0.0, 1.0, 0.0, 1000, STATE_FALLBACK_ONLY)
    
    def _fade(self, alpha_start, alpha_end, vol_start, vol_end, duration_ms, next_state):
        """Generic fade helper using GLib timeouts."""
        steps = 10
        step_ms = max(1, duration_ms // steps)
        step = {"i": 0}
        
        def step_cb():
            i = step["i"]
            t = i / float(steps)
            alpha = alpha_start + (alpha_end - alpha_start) * t
            vol = vol_start + (vol_end - vol_start) * t
            
            if self.cam_alpha:
                self.cam_alpha.set_property("alpha", alpha)
            if self.cam_vol:
                self.cam_vol.set_property("volume", vol)
            
            step["i"] += 1
            if step["i"] > steps:
                # Set final values
                if self.cam_alpha:
                    self.cam_alpha.set_property("alpha", alpha_end)
                if self.cam_vol:
                    self.cam_vol.set_property("volume", vol_end)
                
                self.state = next_state
                print(f"[fade] Fade finished, new state: {self.state}", flush=True)
                
                # If we faded out, schedule SRT restart
                if next_state == STATE_FALLBACK_ONLY and self.srt_ever_connected:
                    if not self.restart_scheduled:
                        self.restart_scheduled = True
                        GLib.timeout_add_seconds(2, self.restart_srt_elements)
                
                return False
            return True
        
        GLib.timeout_add(step_ms, step_cb)
    
    def watchdog_cb(self):
        """Check if SRT has stopped sending data and trigger fade-out."""
        now = time.time()
        delta = now - self.last_srt_buf_time
        
        if self.state in (STATE_SRT_CONNECTED, STATE_SRT_TRANSITIONING) and delta > 1.0:
            # No SRT data for >1 second
            print(f"[watchdog] No SRT data for {delta:.1f}s, triggering fade-out", flush=True)
            self.start_fade_out()
        
        return True  # Keep timeout running
    
    def on_message(self, bus, msg):
        """Handle pipeline bus messages."""
        t = msg.type
        
        if t == Gst.MessageType.ERROR:
            err, debug = msg.parse_error()
            err_domain = err.domain
            err_message = str(err.message)
            
            if "gst-resource-error-quark" in err_domain and "SRT socket" in err_message:
                # Expected SRT disconnection
                print(f"[bus] INFO: SRT client disconnected - will restart after fade", flush=True)
            else:
                # Real error
                print(f"[bus] ERROR: {err} | {debug}", flush=True)
                
        elif t == Gst.MessageType.EOS:
            print("[bus] EOS received (SRT EOF), triggering fade-out", flush=True)
            self.start_fade_out()
            
        elif t == Gst.MessageType.WARNING:
            warn, debug = msg.parse_warning()
            warn_domain = warn.domain
            warn_message = str(warn.message)
            
            if "gst-resource-error-quark" in warn_domain and "SRT socket" in warn_message:
                # Expected SRT disconnection warning
                print(f"[bus] INFO: SRT socket warning - will restart after fade", flush=True)
            else:
                print(f"[bus] WARNING: {warn} | {debug}", flush=True)
    
    def start_pipeline(self):
        """Build and start the complete pipeline."""
        print(f"[compositor] Starting v{__version__}...", flush=True)
        
        # Build all stages
        self._build_fallback_sources()
        self._build_output_stage()
        self._link_fallback_to_output()
        
        # DIAGNOSTIC: Add state change handler before starting
        def on_state_change(bus, msg):
            if msg.type == Gst.MessageType.STATE_CHANGED:
                if msg.src == self.pipeline:
                    old, new, pending = msg.parse_state_changed()
                    print(f"[state] Pipeline: {old.value_nick} -> {new.value_nick} (pending: {pending.value_nick})", flush=True)
                elif msg.src.get_name() in ['mux', 'x264', 'aac', 'black_src', 'silence_src']:
                    old, new, pending = msg.parse_state_changed()
                    print(f"[state] {msg.src.get_name()}: {old.value_nick} -> {new.value_nick}", flush=True)
        
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message::state-changed", on_state_change)
        bus.connect("message", self.on_message)
        
        # Start pipeline with fallback
        print("[compositor] Setting pipeline to PLAYING...", flush=True)
        ret = self.pipeline.set_state(Gst.State.PLAYING)
        print(f"[compositor] set_state(PLAYING) returned: {ret.value_nick}", flush=True)
        
        # Wait for state change to complete
        ret, state, pending = self.pipeline.get_state(5 * Gst.SECOND)
        print(f"[compositor] get_state result: {ret.value_nick}, state={state.value_nick}, pending={pending.value_nick}", flush=True)
        
        if ret == Gst.StateChangeReturn.SUCCESS or ret == Gst.StateChangeReturn.ASYNC:
            print("[compositor] ✓ Pipeline PLAYING with fallback output", flush=True)
        else:
            print(f"[compositor] ⚠ Pipeline state change issue: {ret.value_nick}", flush=True)
            
        print("[compositor] TCP output: tcp://0.0.0.0:5000", flush=True)
        
        # DIAGNOSTIC: Log pipeline clock info
        clock = self.pipeline.get_clock()
        if clock:
            clock_time = clock.get_time()
            base_time = self.pipeline.get_base_time()
            print(f"[clock] Pipeline clock time: {clock_time/Gst.SECOND:.3f}s, base_time: {base_time/Gst.SECOND:.3f}s", flush=True)
        
        # Add SRT elements (will listen for connections)
        self.add_srt_elements()
        
        # Start watchdog
        GLib.timeout_add(200, self.watchdog_cb)
        
        # DIAGNOSTIC: Add periodic status check
        def status_check():
            """Periodic status check to see if data is flowing."""
            mux = self.output_elements.get('mpegtsmux')
            tcp = self.output_elements.get('tcp_sink')
            if mux and tcp:
                mux_state = mux.get_state(0)
                tcp_state = tcp.get_state(0)
                print(f"[status] mux state={mux_state[1].value_nick}, tcp state={tcp_state[1].value_nick}", flush=True)
            return True
        
        GLib.timeout_add_seconds(3, status_check)
        
        print("[compositor] ✓ Ready - streaming black screen, waiting for SRT on port 1937", flush=True)
    
    def run(self):
        """Run the main loop."""
        loop = GLib.MainLoop()
        try:
            loop.run()
        except KeyboardInterrupt:
            print("\n[compositor] Shutting down...", flush=True)
        finally:
            self.pipeline.set_state(Gst.State.NULL)


def main():
    compositor = CompositorManager()
    compositor.start_pipeline()
    compositor.run()


if __name__ == "__main__":
    main()