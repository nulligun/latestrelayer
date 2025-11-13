"""Main stream switcher coordinating FFmpeg processes and GStreamer switching."""

import sys
import time
from datetime import datetime
from typing import Dict, Any

import gi
gi.require_version('Gst', '1.0')
gi.require_version('GObject', '2.0')
from gi.repository import Gst, GObject, GLib

from config import StreamConfig
from ffmpeg_process import FFmpegProcessManager
from gstreamer_simple import SimplifiedPipelineBuilder
from srt_monitor import SRTMonitor


class StreamSwitcher:
    """Main class coordinating the FFmpeg+GStreamer stream switching system.
    
    Manages:
    - Two FFmpeg processes (offline video and SRT input) for normalization
    - Simplified GStreamer pipeline with TCP output for local testing
    - TCP server for local VLC testing (always available)
    - SRT connection monitoring and automatic switching
    - Scene modes (camera/privacy)
    """
    
    def __init__(self, config: StreamConfig):
        """Initialize stream switcher with configuration.
        
        Args:
            config: Stream configuration
        """
        print("=" * 80, flush=True)
        print("🚀 V4.2 - TCP-ONLY ARCHITECTURE - LOCAL VLC TESTING 🚀", flush=True)
        print("=" * 80, flush=True)
        print("[init] Building FFmpeg+GStreamer hybrid architecture...", flush=True)
        
        self.config = config
        self.pipeline = Gst.Pipeline.new("srt-switcher-hybrid")
        
        # State tracking
        self.current_source = "offline"
        self.srt_connected = False
        self.startup_time = datetime.now()
        self.pipeline_playing = False
        self.scene_mode = "camera"  # "camera" (auto-switch) or "privacy" (always offline)
        
        # Pipeline elements
        self.video_selector = None
        self.audio_selector = None
        self.offline_video_pad = None
        self.offline_audio_pad = None
        self.srt_video_pad = None
        self.srt_audio_pad = None
        self.tcpserversink = None
        
        # FFmpeg process manager
        self.ffmpeg_mgr = FFmpegProcessManager(config)
        
        # Set up bus monitoring
        self._setup_bus_monitoring()
        
        # Start FFmpeg input processes
        self._start_ffmpeg_processes()
        
        # Build GStreamer pipeline
        self._build_pipeline()
        
        # Set initial source (will be set once pads are available)
        print("[init] Initial source will be set to offline once pipeline starts", flush=True)
        
        print("[init] ✓ Hybrid pipeline construction complete", flush=True)
    
    def _setup_bus_monitoring(self) -> None:
        """Configure GStreamer bus message monitoring."""
        self.bus = self.pipeline.get_bus()
        self.bus.add_signal_watch()
        self.bus.connect("message", self._on_bus_message)
    
    def _start_ffmpeg_processes(self) -> None:
        """Start FFmpeg input processes (offline and SRT only)."""
        print("[init] Starting FFmpeg input processes...", flush=True)
        
        # Start offline video loop
        if not self.ffmpeg_mgr.start_offline():
            raise RuntimeError("Failed to start offline FFmpeg process")
        
        # Start SRT listener
        if not self.ffmpeg_mgr.start_srt():
            raise RuntimeError("Failed to start SRT FFmpeg process")
        
        # Give FFmpeg a moment to initialize
        time.sleep(0.5)
        
        print("[init] ✓ FFmpeg input processes started", flush=True)
    
    def _build_pipeline(self) -> None:
        """Build simplified GStreamer pipeline with direct RTMP output."""
        # Get file descriptors from FFmpeg input processes
        offline_fd = self.ffmpeg_mgr.get_offline_fd()
        srt_fd = self.ffmpeg_mgr.get_srt_fd()
        
        if offline_fd is None or srt_fd is None:
            raise RuntimeError("Failed to get FFmpeg input file descriptors")
        
        print(f"[init] FFmpeg FDs: offline={offline_fd}, srt={srt_fd}", flush=True)
        
        # Build simplified pipeline with direct RTMP output
        builder = SimplifiedPipelineBuilder(self.pipeline, self.config)
        builder.build(offline_fd, srt_fd)
        
        # Store references to elements
        self.video_selector = builder.get_video_selector()
        self.audio_selector = builder.get_audio_selector()
        self.tcpserversink = builder.get_tcpserversink()
        
        # Pads will be set via callbacks after demux
        self.builder = builder  # Keep reference to get pads later
        
        # Add data probe for SRT monitoring
        builder.add_data_probe("srt", self._on_srt_data_received)
        
        # Set FFmpeg SRT data callback
        self.ffmpeg_mgr.set_srt_data_callback(self._on_srt_data_received)
        
        # Create SRT monitor
        self.srt_monitor = SRTMonitor(
            timeout=self.config.srt_timeout,
            on_connected=self._on_srt_connected,
            on_disconnected=self._on_srt_disconnected,
            interval=self.config.srt_monitor_interval
        )
    
    def _on_srt_data_received(self) -> None:
        """Callback when SRT data flows through the pipeline."""
        self.srt_monitor.update_packet_time()
    
    def _on_bus_message(self, bus: Gst.Bus, msg: Gst.Message) -> None:
        """Handle GStreamer bus messages."""
        msg_type = msg.type
        
        if msg_type == Gst.MessageType.ERROR:
            self._handle_error_message(msg)
        elif msg_type == Gst.MessageType.WARNING:
            self._handle_warning_message(msg)
        elif msg_type == Gst.MessageType.STATE_CHANGED:
            self._handle_state_changed_message(msg)
        elif msg_type == Gst.MessageType.EOS:
            self._handle_eos_message(msg)
        elif msg_type == Gst.MessageType.ELEMENT:
            self._handle_element_message(msg)
    
    def _handle_error_message(self, msg: Gst.Message) -> None:
        """Handle error messages from GStreamer."""
        err, debug = msg.parse_error()
        src = msg.src.get_name() if msg.src else "unknown"
        
        # Check if it's an RTMP error 
        if "rtmp" in src.lower() or "rtmp" in str(err).lower():
            print(f"[RTMP ERROR] {src}: {err}", file=sys.stderr, flush=True)
        else:
            print(f"[ERROR] {src}: {err}", file=sys.stderr, flush=True)
        
        if debug:
            print(f"[DEBUG] {debug}", file=sys.stderr, flush=True)
    
    def _handle_warning_message(self, msg: Gst.Message) -> None:
        """Handle warning messages from GStreamer."""
        warn, debug = msg.parse_warning()
        src = msg.src.get_name() if msg.src else "unknown"
        print(f"[WARN] {src}: {warn}", flush=True)
    
    def _handle_state_changed_message(self, msg: Gst.Message) -> None:
        """Handle pipeline state changes."""
        if msg.src == self.pipeline:
            old, new, pending = msg.parse_state_changed()
            print(f"[pipeline] State: {old.value_nick} → {new.value_nick}", flush=True)
            if new == Gst.State.PLAYING:
                self.pipeline_playing = True
                print("[pipeline] ✓✓✓ Pipeline is PLAYING ✓✓✓", flush=True)
                # Set initial source to offline once playing
                GLib.timeout_add(100, self._set_initial_source)
    
    def _handle_eos_message(self, msg: Gst.Message) -> None:
        """Handle end-of-stream messages."""
        src = msg.src.get_name() if msg.src else "unknown"
        print(f"[bus] EOS from {src}", flush=True)
    
    def _handle_element_message(self, msg: Gst.Message) -> None:
        """Handle element-specific messages (including RTMP status)."""
        structure = msg.get_structure()
        if structure:
            name = structure.get_name()
            src = msg.src.get_name() if msg.src else "unknown"
            
            # Check for RTMP-related messages
            if "rtmp" in src.lower() or "rtmp" in name.lower():
                print(f"[RTMP] {src}: {name}", flush=True)
                
                # Log structure values for debugging
                for i in range(structure.n_fields()):
                    field_name = structure.nth_field_name(i)
                    print(f"[RTMP]   {field_name}: {structure.get_value(field_name)}", flush=True)
    
    def _set_initial_source(self) -> bool:
        """Set initial source to offline after pipeline starts."""
        self._switch_to_offline()
        return False  # Don't repeat
    
    def _on_srt_connected(self) -> bool:
        """Called when SRT feed connects."""
        if not self.srt_connected:
            self.srt_connected = True
            print(f"[auto] SRT feed connected (mode={self.scene_mode})", flush=True)
            
            if self.scene_mode == "camera":
                print("[auto] Camera mode: switching to live source", flush=True)
                self._switch_to_srt()
                # Try to switch, but retry if pads not ready yet
                self._switch_to_srt_with_retry(retries=4)
            else:
                print("[auto] Privacy mode: staying on offline video", flush=True)
        return False  # Don't repeat this idle callback
    
    def _switch_to_srt_with_retry(self, retries: int = 4) -> None:
        """Switch to SRT with retry logic for pad availability.
        
        Args:
            retries: Number of retry attempts remaining
        """
        # Get current pads
        srt_video_pad, srt_audio_pad = self.builder.get_srt_pads()
        
        if srt_video_pad and srt_audio_pad:
            # Both pads ready - perform switch
            self._switch_to_srt()
        elif retries > 0:
            # Pads not ready yet - schedule retry in 250ms
            print(f"[switch] Pads not ready, retrying in 250ms ({retries} attempts left)...", flush=True)
            GLib.timeout_add(250, lambda: self._switch_to_srt_with_retry(retries - 1) or False)
        else:
            # Out of retries
            print("[switch] ERROR: SRT pads failed to become available after 1 second", flush=True)
    
    def _on_srt_disconnected(self) -> bool:
        """Called when SRT feed disconnects."""
        if self.srt_connected:
            self.srt_connected = False
            print(f"[auto] SRT feed disconnected (mode={self.scene_mode})", flush=True)
            
            if self.scene_mode == "camera":
                print("[auto] Camera mode: switching to offline video", flush=True)
                self._switch_to_offline()
            else:
                print("[auto] Privacy mode: already on offline video", flush=True)
        return False  # Don't repeat this idle callback
    
    def _switch_to_srt(self) -> None:
        """Switch selectors to SRT source."""
        # Get current pads
        srt_video_pad, srt_audio_pad = self.builder.get_srt_pads()
        
        if srt_video_pad and srt_audio_pad:
            self.video_selector.set_property("active-pad", srt_video_pad)
            self.audio_selector.set_property("active-pad", srt_audio_pad)
            self.current_source = "srt"
            print(f"[switch] ✓ Active source: SRT (video:{srt_video_pad.get_name()}, audio:{srt_audio_pad.get_name()})", flush=True)
        else:
            print("[switch] Warning: SRT pads not available yet", flush=True)
    
    def _switch_to_offline(self) -> None:
        """Switch selectors to offline source."""
        # Get current pads
        offline_video_pad, offline_audio_pad = self.builder.get_offline_pads()
        
        if offline_video_pad and offline_audio_pad:
            self.video_selector.set_property("active-pad", offline_video_pad)
            self.audio_selector.set_property("active-pad", offline_audio_pad)
            self.current_source = "offline"
            print(f"[switch] ✓ Active source: Offline (video:{offline_video_pad.get_name()}, audio:{offline_audio_pad.get_name()})", flush=True)
        else:
            print("[switch] Warning: Offline pads not available yet", flush=True)
    
    def manual_switch(self, source: str) -> bool:
        """Manually switch to a specific source.
        
        Args:
            source: Source to switch to ('srt', 'fallback', or 'offline')
            
        Returns:
            True if switch succeeded
            
        Raises:
            ValueError: If source is invalid
        """
        print(f"[manual] Manual switch requested to: {source}", flush=True)
        
        if source == "srt":
            self._switch_to_srt()
            return True
        elif source in ["fallback", "offline"]:
            self._switch_to_offline()
            return True
        else:
            raise ValueError(f"Unknown source: {source}")
    
    def set_scene_mode(self, mode: str) -> bool:
        """Set the scene mode: 'camera' or 'privacy'.
        
        Camera mode: Automatically switch between SRT and offline based on connection
        Privacy mode: Always show offline video regardless of SRT connection
        
        Args:
            mode: Scene mode ('camera' or 'privacy')
            
        Returns:
            True if mode change succeeded
            
        Raises:
            ValueError: If mode is invalid
        """
        if mode not in ["camera", "privacy"]:
            raise ValueError(f"Invalid scene mode: {mode}")
        
        old_mode = self.scene_mode
        self.scene_mode = mode
        print(f"[scene] Mode changed: {old_mode} → {mode}", flush=True)
        
        if mode == "privacy":
            print("[scene] Privacy mode: forcing offline video", flush=True)
            self._switch_to_offline()
        elif mode == "camera" and self.srt_connected:
            print("[scene] Camera mode: switching to SRT (connected)", flush=True)
            self._switch_to_srt()
        elif mode == "camera" and not self.srt_connected:
            print("[scene] Camera mode: using offline (SRT not connected)", flush=True)
            self._switch_to_offline()
        
        return True
    
    def start(self) -> bool:
        """Start the GStreamer pipeline and monitoring.
        
        Returns:
            True if started successfully
        """
        print("[main] Starting GStreamer pipeline...", flush=True)
        
        ret = self.pipeline.set_state(Gst.State.PLAYING)
        
        if ret == Gst.StateChangeReturn.FAILURE:
            print("[ERROR] Failed to start pipeline!", file=sys.stderr, flush=True)
            return False
        
        if ret == Gst.StateChangeReturn.ASYNC:
            print("[main] Waiting for pipeline to reach PLAYING state...", flush=True)
            change_return, state, pending = self.pipeline.get_state(5 * Gst.SECOND)
            if change_return == Gst.StateChangeReturn.SUCCESS:
                print(f"[main] ✓ Pipeline reached {state.value_nick} state", flush=True)
        
        # Start SRT monitor
        self.srt_monitor.start()
        
        print("[main] ✓ Pipeline and monitor started", flush=True)
        return True
    
    def stop(self) -> None:
        """Stop the pipeline and monitoring gracefully."""
        print("[shutdown] Stopping monitor...", flush=True)
        self.srt_monitor.stop()
        
        print("[shutdown] Stopping GStreamer pipeline...", flush=True)
        self.pipeline.send_event(Gst.Event.new_eos())
        time.sleep(1)
        self.pipeline.set_state(Gst.State.NULL)
        
        print("[shutdown] Stopping FFmpeg processes...", flush=True)
        self.ffmpeg_mgr.stop_all()
        
        print("[shutdown] ✓ All components stopped", flush=True)
    
    def get_status(self) -> Dict[str, Any]:
        """Get current status for health check.
        
        Returns:
            Dictionary with current state information
        """
        uptime = (datetime.now() - self.startup_time).total_seconds()
        
        return {
            "status": "healthy" if self.pipeline_playing else "starting",
            "current_source": self.current_source,
            "srt_connected": self.srt_connected,
            "scene_mode": self.scene_mode,
            "pipeline_state": self.pipeline.get_state(0)[1].value_nick,
            "uptime_seconds": int(uptime),
            "ffmpeg_processes": self.ffmpeg_mgr.get_status()
        }