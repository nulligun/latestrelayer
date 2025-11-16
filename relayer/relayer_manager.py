#!/usr/bin/env python3
"""
Relayer Manager - Main Orchestrator
Coordinates all relayer components and manages the GStreamer pipeline lifecycle.
"""
import signal
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gst, GLib

from config import (
    __version__,
    FALLBACK_SOURCE,
    WATCHDOG_INTERVAL_MS,
    OUTPUT_TCP_PORT,
    SRT_PORT,
    ENABLE_CPU_PROFILING,
)
from state_manager import StateManager
from privacy_manager import PrivacyManager
from pipeline_builder import PipelineBuilder
from video_source_manager import VideoSourceManager
from srt_source_manager import SRTSourceManager

# Import profiling utilities
try:
    from profiler import CPUMonitor, print_timing_report
    PROFILER_AVAILABLE = True
except ImportError:
    print("[compositor] Warning: profiler.py not found, profiling disabled", flush=True)
    PROFILER_AVAILABLE = False

Gst.init(None)


class RelayerManager:
    """Orchestrates all relayer components and manages pipeline lifecycle."""
    
    def __init__(self):
        """Initialize relayer manager and all sub-managers."""
        self.pipeline = Gst.Pipeline.new("input_selector_compositor")
        
        # Initialize state manager
        self.state_manager = StateManager()
        
        # Initialize pipeline builder
        self.builder = PipelineBuilder(self.pipeline)
        
        # Initialize privacy manager (needs callback for switching)
        self.privacy_manager = PrivacyManager(
            self.state_manager,
            self._switch_to_fallback
        )
        
        # Initialize source managers (will be set after builder creates output)
        self.video_manager = None
        self.srt_manager = None
        
        # MainLoop reference for signal handling
        self.loop = None
        
        # CPU monitoring
        self.cpu_monitor = None
        
        # Store selector pads for switching
        self.selector_pads = None
    
    def _switch_to_fallback(self):
        """Switch to fallback (black screen + silence)."""
        if not self.selector_pads:
            print("[switch] ERROR: Cannot switch to fallback - selector_pads not initialized!", flush=True)
            print("[switch] This indicates pipeline_builder.link_fallback_to_selectors() failed during startup", flush=True)
            return
        
        # Validate pad references
        video_black_pad = self.selector_pads.get('video_black_pad')
        audio_silence_pad = self.selector_pads.get('audio_silence_pad')
        
        if not video_black_pad or not audio_silence_pad:
            print("[switch] ERROR: Selector pads are incomplete!", flush=True)
            print(f"[switch] video_black_pad: {video_black_pad}, audio_silence_pad: {audio_silence_pad}", flush=True)
            return
        
        video_selector = self.builder.get_output_elements()['video_selector']
        audio_selector = self.builder.get_output_elements()['audio_selector']
        
        print("[switch] Switching to FALLBACK (black screen + silence) (instant)", flush=True)
        video_selector.set_property("active-pad", video_black_pad)
        audio_selector.set_property("active-pad", audio_silence_pad)
        
        self.state_manager.set_state("FALLBACK_ONLY")
        print(f"[switch] ✓ Switched to FALLBACK, state={self.state_manager.state}", flush=True)
    
    def _switch_to_video_or_fallback(self):
        """Switch to video if available, otherwise fallback."""
        if self.video_manager and self.video_manager.video_elements and self.state_manager.video_ever_connected:
            self.video_manager._switch_to_video()
        else:
            self._switch_to_fallback()
    
    def start_pipeline(self):
        """Build and start the complete pipeline."""
        print(f"[compositor] Starting v{__version__} with INPUT-SELECTOR architecture...", flush=True)
        
        # Build pipeline stages
        self.builder.build_fallback_sources()
        self.builder.build_output_stage()
        self.builder.link_fallback_to_selectors()
        
        # Get selector pads
        self.selector_pads = self.builder.get_selector_pads()
        output_elements = self.builder.get_output_elements()
        
        # Store pads in output_elements for video manager access
        output_elements['video_black_pad'] = self.selector_pads['video_black_pad']
        output_elements['audio_silence_pad'] = self.selector_pads['audio_silence_pad']
        
        # Initialize source managers now that output is ready
        self.video_manager = VideoSourceManager(
            self.pipeline,
            self.state_manager,
            output_elements
        )
        
        self.srt_manager = SRTSourceManager(
            self.pipeline,
            self.state_manager,
            output_elements,
            self.privacy_manager
        )
        
        # Set callback for SRT to switch to video/fallback
        self.srt_manager.set_fallback_callback(self._switch_to_video_or_fallback)
        
        # Setup bus
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_message)
        
        # Start pipeline
        print("[compositor] Setting pipeline to PLAYING...", flush=True)
        ret = self.pipeline.set_state(Gst.State.PLAYING)
        print(f"[compositor] set_state(PLAYING) returned: {ret.value_nick}", flush=True)
        
        ret, state, pending = self.pipeline.get_state(5 * Gst.SECOND)
        print(f"[compositor] get_state result: {ret.value_nick}, state={state.value_nick}", flush=True)
        
        if ret == Gst.StateChangeReturn.SUCCESS or ret == Gst.StateChangeReturn.ASYNC:
            print("[compositor] ✓ Pipeline PLAYING with fallback output", flush=True)
        else:
            print(f"[compositor] ⚠ Pipeline state change issue: {ret.value_nick}", flush=True)
        
        print(f"[compositor] TCP output: tcp://0.0.0.0:{OUTPUT_TCP_PORT}", flush=True)
        
        # Add video fallback if configured
        if FALLBACK_SOURCE == "video":
            print(f"[compositor] FALLBACK_SOURCE=video, adding TCP video server", flush=True)
            self.video_manager.add_video_elements()
            GLib.timeout_add(WATCHDOG_INTERVAL_MS, self.video_manager.video_watchdog_cb)
        
        # Add SRT elements
        self.srt_manager.add_srt_elements()
        
        # Start SRT watchdog
        GLib.timeout_add(WATCHDOG_INTERVAL_MS, self.srt_manager.watchdog_cb)
        
        if FALLBACK_SOURCE == "video":
            print(f"[compositor] ✓ Ready - black screen active, waiting for video and SRT (:{ SRT_PORT})", flush=True)
        else:
            print(f"[compositor] ✓ Ready - black screen active, waiting for SRT on port {SRT_PORT}", flush=True)
    
    def on_message(self, bus, msg):
        """Handle pipeline bus messages."""
        t = msg.type
        src = msg.src.get_name() if msg.src else "unknown"
        
        if t == Gst.MessageType.ERROR:
            err, debug = msg.parse_error()
            err_message = str(err.message)
            
            if "TCP" in err_message and self.video_manager and self.video_manager.video_elements:
                print(f"[bus] INFO: TCP video client disconnected - will restart after switch", flush=True)
            elif "SRT socket" in err_message:
                print(f"[bus] INFO: SRT client disconnected - will restart after grace period", flush=True)
            else:
                print(f"[bus] ERROR from {src}: {err} | {debug}", flush=True)
        
        elif t == Gst.MessageType.EOS:
            print(f"[bus] EOS received from {src}", flush=True)
            if self.state_manager.state == "SRT_CONNECTED":
                self.srt_manager._schedule_srt_disconnect_grace()
            elif self.state_manager.state == "VIDEO_CONNECTED":
                self._switch_to_fallback()
        
        elif t == Gst.MessageType.WARNING:
            warn, debug = msg.parse_warning()
            warn_message = str(warn.message)
            
            if "TCP" not in warn_message and "SRT" not in warn_message:
                print(f"[bus] WARNING from {src}: {warn}", flush=True)
    
    def run(self):
        """Run the main loop."""
        self.loop = GLib.MainLoop()
        
        # Start CPU monitoring if profiling is enabled
        if ENABLE_CPU_PROFILING and PROFILER_AVAILABLE:
            self.cpu_monitor = CPUMonitor(self)
            self.cpu_monitor.start(interval_seconds=10)
            print("[compositor] ✓ CPU profiling enabled", flush=True)
        else:
            print(f"[compositor] CPU profiling disabled", flush=True)
        
        # Signal handler for graceful shutdown
        def signal_handler(signum, frame):
            sig_name = signal.Signals(signum).name
            print(f"\n[compositor] Received {sig_name}, shutting down gracefully...", flush=True)
            if self.cpu_monitor:
                self.cpu_monitor.stop()
                if PROFILER_AVAILABLE:
                    print_timing_report()
            if self.loop:
                self.loop.quit()
        
        signal.signal(signal.SIGTERM, signal_handler)
        signal.signal(signal.SIGINT, signal_handler)
        print("[compositor] Signal handlers registered (SIGTERM, SIGINT)", flush=True)
        
        try:
            self.loop.run()
        except KeyboardInterrupt:
            print("\n[compositor] Shutting down...", flush=True)
        finally:
            if self.cpu_monitor:
                self.cpu_monitor.stop()
                if PROFILER_AVAILABLE:
                    print_timing_report()
            print("[compositor] Cleaning up pipeline...", flush=True)
            self.pipeline.set_state(Gst.State.NULL)
            print("[compositor] ✓ Shutdown complete", flush=True)
    
    # API Methods for HTTP handler
    
    def get_current_scene(self):
        """Get the current scene name."""
        return self.state_manager.get_current_scene()
    
    def get_srt_stats(self):
        """Get SRT connection statistics."""
        return self.srt_manager.get_srt_stats()
    
    def get_privacy_enabled(self):
        """Get privacy mode status."""
        return self.privacy_manager.is_enabled()
    
    def set_privacy_mode(self, enabled):
        """Set privacy mode."""
        self.privacy_manager.set_privacy_mode(enabled)
        
        # Check if SRT should be activated after privacy is disabled
        if not enabled:
            self.srt_manager.check_srt_reconnect()