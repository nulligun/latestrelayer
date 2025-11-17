#!/usr/bin/env python3
"""
Video Source Manager for Compositor
Manages TCP video source elements and their lifecycle.
"""
import time
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gst, GLib

from config import (
    VIDEO_TCP_PORT,
    VIDEO_WIDTH,
    VIDEO_HEIGHT,
    VIDEO_QUEUE_MAX_TIME,
    COMP_QUEUE_MAX_TIME,
    BUFFER_DELAY_MS,
    VIDEO_WATCHDOG_TIMEOUT,
    ELEMENT_REMOVAL_TIMEOUT_SEC,
    STATE_FALLBACK_ONLY,
    STATE_VIDEO_BUFFERING,
    STATE_VIDEO_CONNECTED,
)

# Import profiling if available
try:
    from profiler import profile_timing
    PROFILER_AVAILABLE = True
except ImportError:
    PROFILER_AVAILABLE = False
    def profile_timing(func):
        return func


class VideoSourceManager:
    """Manages TCP video source elements and lifecycle."""
    
    def __init__(self, pipeline, state_manager, output_elements):
        """
        Initialize video source manager.
        
        Args:
            pipeline: GStreamer Pipeline object
            state_manager: StateManager instance
            output_elements: Dictionary of output elements from PipelineBuilder
        """
        self.pipeline = pipeline
        self.state_manager = state_manager
        self.output_elements = output_elements
        
        # Video elements
        self.video_elements = None
        
        # Selector pad references
        self.video_selector_video_pad = None
        self.audio_selector_video_pad = None
    
    def add_video_elements(self):
        """Add TCP video server elements to the running pipeline (remux-only, no decoding/encoding)."""
        if self.video_elements:
            print("[video] Video elements already added", flush=True)
            return
        
        print("[video] Adding video TCP server elements to pipeline (remux mode)...", flush=True)
        
        # Create TCP video server chain
        tcp_src = Gst.ElementFactory.make("tcpserversrc", "tcp_src")
        tcp_src.set_property("host", "0.0.0.0")
        tcp_src.set_property("port", VIDEO_TCP_PORT)
        tcp_src.set_property("do-timestamp", True)
        
        # Note: TCP connection/disconnection is handled by the watchdog mechanism
        # The watchdog monitors data flow and triggers restarts when needed
        
        # Use tsdemux instead of decodebin (no decoding, just demux MPEG-TS container)
        tsdemux = Gst.ElementFactory.make("tsdemux", "video_tsdemux")
        
        # Video chain - parse H.264 (no decode/convert/scale/rate needed)
        h264parse = Gst.ElementFactory.make("h264parse", "video_h264parse")
        
        video_queue = Gst.ElementFactory.make("queue", "video_video_q")
        video_queue.set_property("max-size-time", VIDEO_QUEUE_MAX_TIME)
        video_queue.set_property("max-size-buffers", 0)
        video_queue.set_property("leaky", 2)
        
        video_comp_queue = Gst.ElementFactory.make("queue", "video_comp_q")
        video_comp_queue.set_property("max-size-time", COMP_QUEUE_MAX_TIME)
        video_comp_queue.set_property("max-size-buffers", 0)
        video_comp_queue.set_property("leaky", 2)
        
        # Audio chain - parse AAC (no convert/resample needed)
        aacparse = Gst.ElementFactory.make("aacparse", "video_aacparse")
        
        audio_queue = Gst.ElementFactory.make("queue", "video_audio_q")
        audio_queue.set_property("max-size-time", VIDEO_QUEUE_MAX_TIME)
        audio_queue.set_property("max-size-buffers", 0)
        audio_queue.set_property("leaky", 2)
        
        audio_out_queue = Gst.ElementFactory.make("queue", "video_audio_out_q")
        audio_out_queue.set_property("max-size-time", VIDEO_QUEUE_MAX_TIME)
        audio_out_queue.set_property("max-size-buffers", 0)
        audio_out_queue.set_property("leaky", 2)  # Critical: prevent blocking on reconnect
        
        self.video_elements = {
            'tcp_src': tcp_src,
            'tsdemux': tsdemux,
            'h264parse': h264parse,
            'video_queue': video_queue,
            'video_comp_queue': video_comp_queue,
            'aacparse': aacparse,
            'audio_queue': audio_queue,
            'audio_out_queue': audio_out_queue,
        }
        
        # Add to pipeline
        for elem in self.video_elements.values():
            self.pipeline.add(elem)
        
        # Link TCP → tsdemux (tsdemux will create pads dynamically)
        tcp_src.link(tsdemux)
        
        # Link video chain: h264parse → queue → comp_queue
        h264parse.link(video_queue)
        video_queue.link(video_comp_queue)
        
        # Link audio chain: aacparse → queue → out_queue
        aacparse.link(audio_queue)
        audio_queue.link(audio_out_queue)
        
        # Connect signals (tsdemux creates pads dynamically)
        tsdemux.connect("pad-added", self._on_video_pad_added)
        video_queue_src = video_queue.get_static_pad("src")
        video_queue_src.add_probe(Gst.PadProbeType.BUFFER, self._on_video_probe)
        
        # Add queue overflow monitoring
        audio_out_queue_sink = audio_out_queue.get_static_pad("sink")
        audio_out_queue_sink.add_probe(Gst.PadProbeType.BUFFER, self._on_audio_queue_monitor)
        
        # Sync state
        for elem in self.video_elements.values():
            elem.sync_state_with_parent()
        
        print(f"[video] ✓ Video elements added (remux mode), TCP server listening on port {VIDEO_TCP_PORT}", flush=True)
    
    def _on_video_pad_added(self, tsdemux, pad):
        """Handle dynamic pad creation from tsdemux when TCP video connects."""
        caps = pad.get_current_caps()
        name = caps.to_string() if caps else ""
        print(f"[video] tsdemux pad added: {name}", flush=True)
        
        if not self.state_manager.video_ever_connected:
            self.state_manager.video_ever_connected = True
            print("[video] ✓ First TCP video connection successful", flush=True)
        
        video_selector = self.output_elements['video_selector']
        audio_selector = self.output_elements['audio_selector']
        
        if name.startswith("video/"):
            # Link tsdemux video pad to h264parse
            sinkpad = self.video_elements['h264parse'].get_static_pad("sink")
            if not sinkpad.is_linked():
                ret = pad.link(sinkpad)
                print(f"[video] Video pad → h264parse link: {ret}", flush=True)
                
                # Request pad on video selector for video source
                self.video_selector_video_pad = video_selector.request_pad_simple("sink_%u")
                video_comp_queue = self.video_elements['video_comp_queue']
                comp_queue_src = video_comp_queue.get_static_pad("src")
                comp_queue_src.link(self.video_selector_video_pad)
                print(f"[video] ✓ Video linked to selector pad", flush=True)
                
        elif name.startswith("audio/"):
            # Link tsdemux audio pad to aacparse
            sinkpad = self.video_elements['aacparse'].get_static_pad("sink")
            if not sinkpad.is_linked():
                ret = pad.link(sinkpad)
                print(f"[video] Audio pad → aacparse link: {ret}", flush=True)
                
                # Request pad on audio selector for video audio
                self.audio_selector_video_pad = audio_selector.request_pad_simple("sink_%u")
                audio_out_queue = self.video_elements['audio_out_queue']
                audio_queue_src = audio_out_queue.get_static_pad("src")
                audio_queue_src.link(self.audio_selector_video_pad)
                print("[video] ✓ Audio linked to selector pad", flush=True)
    
    @profile_timing
    def _on_video_probe(self, pad, info):
        """Track TCP video buffer flow and trigger buffering/switching."""
        self.state_manager.update_video_buffer_time()
        
        # Start buffering on first buffer
        if self.state_manager.state == STATE_FALLBACK_ONLY:
            if self.state_manager.start_video_buffering():
                print(f"[buffer] Video buffering started, waiting {BUFFER_DELAY_MS}ms before switch...", flush=True)
                # Schedule switch after buffer delay
                if not self.state_manager.video_buffer_scheduled:
                    self.state_manager.video_buffer_scheduled = True
                    GLib.timeout_add(BUFFER_DELAY_MS, self._switch_to_video_after_buffer)
        
        return Gst.PadProbeReturn.OK
    
    def _on_audio_queue_monitor(self, pad, info):
        """Monitor audio queue for potential blocking issues."""
        if self.video_elements and self.video_elements.get('audio_out_queue'):
            queue = self.video_elements['audio_out_queue']
            current_level = queue.get_property("current-level-time")
            max_level = queue.get_property("max-size-time")
            if current_level > max_level * 0.8:  # 80% threshold
                print(f"[queue-monitor] ⚠ audio_out_queue at {current_level/1e9:.1f}s / {max_level/1e9:.1f}s", flush=True)
        return Gst.PadProbeReturn.OK
    
    def _switch_to_video_after_buffer(self):
        """Callback to switch to video after buffer delay."""
        self.state_manager.video_buffer_scheduled = False
        
        if self.state_manager.state == STATE_VIDEO_BUFFERING:
            elapsed = time.time() - (self.state_manager.video_first_buffer_time or 0)
            print(f"[buffer] Video buffering complete ({elapsed*1000:.0f}ms), switching to video", flush=True)
            self._switch_to_video()
        else:
            print(f"[buffer] Video buffer timeout but state changed to {self.state_manager.state}, skipping switch", flush=True)
        
        return False
    
    def _switch_to_video(self):
        """Switch input-selectors to video source."""
        if not self.video_selector_video_pad or not self.audio_selector_video_pad:
            print("[switch] Cannot switch to video - pads not available", flush=True)
            return
        
        video_selector = self.output_elements['video_selector']
        audio_selector = self.output_elements['audio_selector']
        
        print("[switch] Switching to VIDEO source (instant)", flush=True)
        video_selector.set_property("active-pad", self.video_selector_video_pad)
        audio_selector.set_property("active-pad", self.audio_selector_video_pad)
        
        self.state_manager.set_state(STATE_VIDEO_CONNECTED)
        print(f"[switch] ✓ Switched to VIDEO, state={self.state_manager.state}", flush=True)
    
    def remove_video_elements(self):
        """Remove video TCP elements from the pipeline."""
        if not self.video_elements:
            print("[video] No video elements to remove", flush=True)
            return
        
        print("[video] Removing video TCP elements from pipeline...", flush=True)
        
        # First, send EOS to demuxer to flush state
        tsdemux = self.video_elements.get('tsdemux')
        if tsdemux:
            try:
                print("[video] Flushing tsdemux state...", flush=True)
                tsdemux.send_event(Gst.Event.new_flush_start())
                tsdemux.send_event(Gst.Event.new_flush_stop(True))
            except Exception as e:
                print(f"[video] ⚠ Failed to flush tsdemux: {e}", flush=True)
        
        for elem_name, elem in self.video_elements.items():
            try:
                elem.set_state(Gst.State.NULL)
                ret, state, pending = elem.get_state(ELEMENT_REMOVAL_TIMEOUT_SEC * Gst.SECOND)
                
                if ret == Gst.StateChangeReturn.FAILURE:
                    print(f"[video] ⚠ State transition failed for {elem_name}", flush=True)
                elif ret == Gst.StateChangeReturn.ASYNC:
                    print(f"[video] State transition pending for {elem_name}", flush=True)
                
                try:
                    self.pipeline.remove(elem)
                except Exception as remove_err:
                    print(f"[video] ⚠ Failed to remove {elem_name}: {remove_err}", flush=True)
                    
            except Exception as e:
                print(f"[video] ⚠ Exception removing {elem_name}: {e}", flush=True)
        
        self.video_elements = None
        self.video_selector_video_pad = None
        self.audio_selector_video_pad = None
        
        print("[video] ✓ Video elements removed", flush=True)
    
    def restart_video_elements(self):
        """Restart video TCP elements after a timeout."""
        print("[video] Restarting video TCP server...", flush=True)
        self.state_manager.video_restart_scheduled = False
        
        self.remove_video_elements()
        
        # Reset buffer tracking
        self.state_manager.reset_video_buffers()
        
        if self.state_manager.state not in (STATE_FALLBACK_ONLY, STATE_VIDEO_CONNECTED):
            self.state_manager.set_state(STATE_FALLBACK_ONLY)
        
        # Wait for OS to release TCP port
        def delayed_add():
            self.add_video_elements()
            print("[video] ✓ Video TCP server restart complete", flush=True)
            return False
        
        GLib.timeout_add(500, delayed_add)
        return False
    
    @profile_timing
    def video_watchdog_cb(self):
        """Check if TCP video has stopped sending data."""
        now = time.time()
        delta = now - self.state_manager.last_video_buf_time
        
        if self.state_manager.state in (STATE_VIDEO_CONNECTED, STATE_VIDEO_BUFFERING) and delta > VIDEO_WATCHDOG_TIMEOUT:
            if self.state_manager.state == STATE_VIDEO_BUFFERING:
                print(f"[watchdog] No video data during buffering for {delta:.1f}s, cancelling", flush=True)
                self.state_manager.cancel_video_buffering()
            else:
                print(f"[watchdog] No video TCP data for {delta:.1f}s, switching to fallback", flush=True)
                self._switch_to_fallback()
                
                # Trigger TCP server restart to ensure clean reconnection
                if not self.state_manager.video_restart_scheduled:
                    print("[watchdog] Scheduling TCP server restart for clean reconnection...", flush=True)
                    self.state_manager.video_restart_scheduled = True
                    # Schedule restart after 500ms delay
                    GLib.timeout_add(500, self.restart_video_elements)
                else:
                    print("[watchdog] TCP server restart already scheduled, skipping", flush=True)
        
        return True
    
    def _switch_to_fallback(self):
        """Switch input-selectors to fallback (black screen + silence)."""
        video_selector = self.output_elements['video_selector']
        audio_selector = self.output_elements['audio_selector']
        
        # Get fallback pads from output elements (dictionary access)
        video_black_pad = self.output_elements.get('video_black_pad', None)
        audio_silence_pad = self.output_elements.get('audio_silence_pad', None)
        
        if not video_black_pad or not audio_silence_pad:
            print("[switch] ERROR: Cannot switch to fallback - pads not available!", flush=True)
            print(f"[switch] video_black_pad: {video_black_pad}, audio_silence_pad: {audio_silence_pad}", flush=True)
            return
        
        print("[switch] Switching to FALLBACK (black screen + silence) (instant)", flush=True)
        video_selector.set_property("active-pad", video_black_pad)
        audio_selector.set_property("active-pad", audio_silence_pad)
        
        self.state_manager.set_state(STATE_FALLBACK_ONLY)
        print(f"[switch] ✓ Switched to FALLBACK, state={self.state_manager.state}", flush=True)
    
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
                    return {'connected': True, 'stats': str(stats), 'current_port': current_port}
                else:
                    return {'connected': False, 'bytes_received': 0, 'current_port': current_port}
        except Exception as e:
            print(f"[tcp-stats] Error getting TCP stats: {e}", flush=True)
        
        return {'connected': False, 'bytes_received': 0, 'current_port': 0}