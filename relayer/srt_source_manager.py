#!/usr/bin/env python3
"""
SRT Source Manager for Compositor
Manages SRT source elements and their lifecycle.
"""
import time
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gst, GLib

from config import (
    SRT_PORT,
    VIDEO_WIDTH,
    VIDEO_HEIGHT,
    SRT_VIDEO_QUEUE_MAX_TIME,
    SRT_AUDIO_QUEUE_MAX_TIME,
    COMP_QUEUE_MAX_TIME,
    BUFFER_DELAY_MS,
    SRT_GRACE_PERIOD_MS,
    SRT_LATENCY_MS,
    ELEMENT_REMOVAL_TIMEOUT_SEC,
    STATE_FALLBACK_ONLY,
    STATE_VIDEO_CONNECTED,
    STATE_SRT_BUFFERING,
    STATE_SRT_CONNECTED,
)

# Import profiling if available
try:
    from profiler import profile_timing
    PROFILER_AVAILABLE = True
except ImportError:
    PROFILER_AVAILABLE = False
    def profile_timing(func):
        return func


class SRTSourceManager:
    """Manages SRT source elements and lifecycle."""
    
    def __init__(self, pipeline, state_manager, output_elements, privacy_manager):
        """
        Initialize SRT source manager.
        
        Args:
            pipeline: GStreamer Pipeline object
            state_manager: StateManager instance
            output_elements: Dictionary of output elements from PipelineBuilder
            privacy_manager: PrivacyManager instance for privacy checks
        """
        self.pipeline = pipeline
        self.state_manager = state_manager
        self.output_elements = output_elements
        self.privacy_manager = privacy_manager
        
        # SRT elements
        self.srt_elements = None
        
        # Selector pad references
        self.video_selector_srt_pad = None
        self.audio_selector_srt_pad = None
        
        # Callback for switching to video/fallback
        self.fallback_callback = None
    
    def set_fallback_callback(self, callback):
        """Set callback for switching to fallback when needed."""
        self.fallback_callback = callback
    
    def add_srt_elements(self):
        """Add SRT source elements to the running pipeline."""
        if self.srt_elements:
            print("[srt] SRT elements already added", flush=True)
            return
        
        print("[srt] Adding SRT elements to pipeline...", flush=True)
        
        srt_src = Gst.ElementFactory.make("srtserversrc", "srt_src")
        srt_src.set_property("uri", f"srt://:{SRT_PORT}?mode=listener")
        srt_src.set_property("latency", SRT_LATENCY_MS)
        
        # Add SRT connection event handlers
        def on_caller_connected(element, socket_fd, addr):
            print(f"[srt-connection] ✓ CLIENT CONNECTED! fd={socket_fd}, addr={addr}", flush=True)
            self.state_manager.srt_connected = True
        
        def on_caller_disconnected(element, socket_fd, addr):
            print(f"[srt-connection] ✗ CLIENT DISCONNECTED! fd={socket_fd}, addr={addr}", flush=True)
            self.state_manager.srt_connected = False
            self.state_manager.srt_bitrate_kbps = 0
        
        try:
            srt_src.connect("caller-added", on_caller_connected)
            srt_src.connect("caller-removed", on_caller_disconnected)
        except Exception as e:
            print(f"[srt-debug] Could not connect signals: {e}", flush=True)
        
        decode = Gst.ElementFactory.make("decodebin", "decode")
        
        video_queue = Gst.ElementFactory.make("queue", "video_q")
        video_queue.set_property("max-size-time", SRT_VIDEO_QUEUE_MAX_TIME)
        video_queue.set_property("max-size-buffers", 0)
        video_queue.set_property("leaky", 2)
        videoconvert = Gst.ElementFactory.make("videoconvert", "vconv")
        videoscale = Gst.ElementFactory.make("videoscale", "vscale")
        videorate = Gst.ElementFactory.make("videorate", "vrate")
        videorate.set_property("drop-only", True)
        
        srt_comp_queue = Gst.ElementFactory.make("queue", "srt_comp_q")
        srt_comp_queue.set_property("max-size-time", COMP_QUEUE_MAX_TIME)
        srt_comp_queue.set_property("max-size-buffers", 0)
        srt_comp_queue.set_property("leaky", 2)
        
        audio_queue = Gst.ElementFactory.make("queue", "audio_q")
        audio_queue.set_property("max-size-time", SRT_AUDIO_QUEUE_MAX_TIME)
        audio_queue.set_property("max-size-buffers", 0)
        audio_queue.set_property("leaky", 2)
        audioconvert = Gst.ElementFactory.make("audioconvert", "aconv")
        audioresample = Gst.ElementFactory.make("audioresample", "ares")
        audio_out_queue = Gst.ElementFactory.make("queue", "srt_audio_out_q")
        
        self.srt_elements = {
            'srt_src': srt_src,
            'decode': decode,
            'video_queue': video_queue,
            'videoconvert': videoconvert,
            'videoscale': videoscale,
            'videorate': videorate,
            'srt_comp_queue': srt_comp_queue,
            'audio_queue': audio_queue,
            'audioconvert': audioconvert,
            'audioresample': audioresample,
            'audio_out_queue': audio_out_queue,
        }
        
        for elem in self.srt_elements.values():
            self.pipeline.add(elem)
        
        srt_src.link(decode)
        video_queue.link(videoconvert)
        videoconvert.link(videoscale)
        videoscale.link(videorate)
        videorate.link(srt_comp_queue)
        audio_queue.link(audioconvert)
        audioconvert.link(audioresample)
        audioresample.link(audio_out_queue)
        
        decode.connect("pad-added", self._on_srt_pad_added)
        video_queue_src = video_queue.get_static_pad("src")
        video_queue_src.add_probe(Gst.PadProbeType.BUFFER, self._on_srt_video_probe)
        
        # Sync states
        for elem in self.srt_elements.values():
            elem.sync_state_with_parent()
        
        # Ensure decodebin and srt src are playing
        decode.set_state(Gst.State.PLAYING)
        srt_src.set_state(Gst.State.PLAYING)
        
        print(f"[srt] ✓ SRT elements added, listening on port {SRT_PORT}", flush=True)
    
    def _on_srt_pad_added(self, decodebin, pad):
        """Handle dynamic pad creation from decodebin when SRT connects."""
        caps = pad.get_current_caps()
        name = caps.to_string() if caps else ""
        print(f"[srt] Decodebin pad added: {name}", flush=True)
        
        if not self.state_manager.srt_ever_connected:
            self.state_manager.srt_ever_connected = True
            self.state_manager.srt_connected = True
            print("[srt] ✓ First SRT connection successful", flush=True)
        
        video_selector = self.output_elements['video_selector']
        audio_selector = self.output_elements['audio_selector']
        
        if name.startswith("video/"):
            sinkpad = self.srt_elements['video_queue'].get_static_pad("sink")
            if not sinkpad.is_linked():
                ret = pad.link(sinkpad)
                print(f"[srt] Video pad link result: {ret}", flush=True)
                
                # Request pad on video selector for SRT source
                self.video_selector_srt_pad = video_selector.request_pad_simple("sink_%u")
                srt_comp_queue = self.srt_elements['srt_comp_queue']
                comp_queue_src = srt_comp_queue.get_static_pad("src")
                comp_queue_src.link(self.video_selector_srt_pad)
                print(f"[srt] ✓ Video linked to selector pad", flush=True)
                
        elif name.startswith("audio/"):
            sinkpad = self.srt_elements['audio_queue'].get_static_pad("sink")
            if not sinkpad.is_linked():
                ret = pad.link(sinkpad)
                print(f"[srt] Audio pad link result: {ret}", flush=True)
                
                # Request pad on audio selector for SRT audio
                self.audio_selector_srt_pad = audio_selector.request_pad_simple("sink_%u")
                audio_out_queue = self.srt_elements['audio_out_queue']
                audio_queue_src = audio_out_queue.get_static_pad("src")
                audio_queue_src.link(self.audio_selector_srt_pad)
                print("[srt] ✓ Audio linked to selector pad", flush=True)
    
    @profile_timing
    def _on_srt_video_probe(self, pad, info):
        """Track SRT buffer flow and trigger buffering/switching."""
        self.state_manager.update_srt_buffer_time()
        
        # Start buffering on first buffer
        if self.state_manager.state in (STATE_FALLBACK_ONLY, STATE_VIDEO_CONNECTED):
            if self.state_manager.start_srt_buffering():
                print(f"[buffer] SRT buffering started, waiting {BUFFER_DELAY_MS}ms before switch...", flush=True)
                # Schedule switch after buffer delay
                if not self.state_manager.srt_buffer_scheduled:
                    self.state_manager.srt_buffer_scheduled = True
                    GLib.timeout_add(BUFFER_DELAY_MS, self._switch_to_srt_after_buffer)
        
        return Gst.PadProbeReturn.OK
    
    def _switch_to_srt_after_buffer(self):
        """Callback to switch to SRT after buffer delay."""
        self.state_manager.srt_buffer_scheduled = False
        
        if self.state_manager.state == STATE_SRT_BUFFERING:
            elapsed = time.time() - (self.state_manager.srt_first_buffer_time or 0)
            print(f"[buffer] SRT buffering complete ({elapsed*1000:.0f}ms), switching to SRT", flush=True)
            self._switch_to_srt()
        else:
            print(f"[buffer] SRT buffer timeout but state changed to {self.state_manager.state}, skipping switch", flush=True)
        
        return False
    
    def _switch_to_srt(self):
        """Switch input-selectors to SRT source (if privacy allows)."""
        # Check privacy mode
        if not self.privacy_manager.allows_srt():
            print("[switch] Cannot switch to SRT - privacy mode is enabled", flush=True)
            return
        
        if not self.video_selector_srt_pad or not self.audio_selector_srt_pad:
            print("[switch] Cannot switch to SRT - pads not available", flush=True)
            return
        
        video_selector = self.output_elements['video_selector']
        audio_selector = self.output_elements['audio_selector']
        
        print("[switch] Switching to SRT source (instant)", flush=True)
        video_selector.set_property("active-pad", self.video_selector_srt_pad)
        audio_selector.set_property("active-pad", self.audio_selector_srt_pad)
        
        self.state_manager.set_state(STATE_SRT_CONNECTED)
        print(f"[switch] ✓ Switched to SRT, state={self.state_manager.state}", flush=True)
    
    def remove_srt_elements(self):
        """Remove SRT elements from the pipeline."""
        if not self.srt_elements:
            print("[srt] No SRT elements to remove", flush=True)
            return
        
        print("[srt] Removing SRT elements from pipeline...", flush=True)
        
        for elem_name, elem in self.srt_elements.items():
            try:
                elem.set_state(Gst.State.NULL)
                ret, state, pending = elem.get_state(ELEMENT_REMOVAL_TIMEOUT_SEC * Gst.SECOND)
                
                if ret == Gst.StateChangeReturn.FAILURE:
                    print(f"[srt] ⚠ State transition failed for {elem_name}", flush=True)
                
                try:
                    self.pipeline.remove(elem)
                except Exception as remove_err:
                    print(f"[srt] ⚠ Failed to remove {elem_name}: {remove_err}", flush=True)
                    
            except Exception as e:
                print(f"[srt] ⚠ Exception removing {elem_name}: {e}", flush=True)
        
        self.srt_elements = None
        self.video_selector_srt_pad = None
        self.audio_selector_srt_pad = None
        self.state_manager.srt_connected = False
        self.state_manager.srt_bitrate_kbps = 0
        
        print("[srt] ✓ SRT elements removed", flush=True)
    
    def restart_srt_elements(self):
        """Restart SRT elements after a timeout."""
        print("[srt] Restarting SRT elements...", flush=True)
        self.state_manager.restart_scheduled = False
        
        self.remove_srt_elements()
        
        # Reset buffer tracking
        self.state_manager.reset_srt_buffers()
        
        self.add_srt_elements()
        
        print("[srt] ✓ SRT restart complete, waiting for connection...", flush=True)
        return False
    
    def _schedule_srt_disconnect_grace(self):
        """Schedule grace period before switching away from SRT."""
        if self.state_manager.srt_disconnect_grace_scheduled:
            return  # Already scheduled
        
        self.state_manager.srt_disconnect_grace_scheduled = True
        print(f"[grace] SRT disconnect detected, starting {SRT_GRACE_PERIOD_MS}ms grace period...", flush=True)
        
        def grace_timeout():
            self.state_manager.srt_disconnect_grace_scheduled = False
            
            # Check if SRT came back during grace period
            now = time.time()
            delta = now - self.state_manager.last_srt_buf_time
            
            if delta > (SRT_GRACE_PERIOD_MS / 1000.0):
                print(f"[grace] Grace period expired, SRT still disconnected, switching away", flush=True)
                
                # Call fallback callback if set
                if self.fallback_callback:
                    self.fallback_callback()
                
                # Schedule SRT restart
                if not self.state_manager.restart_scheduled:
                    self.state_manager.restart_scheduled = True
                    GLib.timeout_add_seconds(2, self.restart_srt_elements)
            else:
                print(f"[grace] SRT reconnected during grace period, keeping active", flush=True)
            
            return False
        
        GLib.timeout_add(SRT_GRACE_PERIOD_MS, grace_timeout)
    
    @profile_timing
    def watchdog_cb(self):
        """Check if SRT has stopped sending data."""
        now = time.time()
        delta = now - self.state_manager.last_srt_buf_time
        
        if self.state_manager.state in (STATE_SRT_CONNECTED, STATE_SRT_BUFFERING) and delta > 0.2:
            if self.state_manager.state == STATE_SRT_BUFFERING:
                print(f"[watchdog] No SRT data during buffering for {delta:.1f}s, cancelling", flush=True)
                self.state_manager.cancel_srt_buffering()
            else:
                print(f"[watchdog] No SRT data for {delta:.1f}s, starting grace period", flush=True)
                self._schedule_srt_disconnect_grace()
        
        return True
    
    def get_srt_stats(self):
        """Get SRT connection statistics including bitrate."""
        if not self.srt_elements or not self.state_manager.srt_connected:
            return {'connected': False, 'bitrate_kbps': 0}
        
        try:
            srt_src = self.srt_elements.get('srt_src')
            if srt_src:
                stats = srt_src.get_property("stats")
                if stats:
                    bitrate_bps = stats.get('bitrate', 0)
                    bitrate_kbps = int(bitrate_bps / 1024) if bitrate_bps else 0
                    return {'connected': True, 'bitrate_kbps': bitrate_kbps}
        except Exception as e:
            print(f"[srt-stats] Error getting SRT stats: {e}", flush=True)
        
        return {'connected': self.state_manager.srt_connected, 'bitrate_kbps': 0}
    
    def check_srt_reconnect(self):
        """Check if SRT is connected but blocked by privacy and should activate."""
        if self.state_manager.srt_connected and self.state_manager.state != STATE_SRT_CONNECTED:
            if self.privacy_manager.allows_srt():
                print("[srt] SRT is connected, activating now that privacy allows", flush=True)
                self._switch_to_srt()