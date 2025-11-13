"""Simplified GStreamer pipeline using pre-normalized FFmpeg inputs with TCP output for local testing."""

import gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst

import os
import time
from typing import Tuple, Callable, Optional
from config import StreamConfig


def make_element(factory: str, name: str) -> Gst.Element:
    """Create a GStreamer element with error checking.
    
    Args:
        factory: Element factory name
        name: Instance name for this element
        
    Returns:
        Created GStreamer element
        
    Raises:
        RuntimeError: If element creation fails
    """
    elem = Gst.ElementFactory.make(factory, name)
    if not elem:
        raise RuntimeError(f"Failed to create element: {factory}")
    return elem


class SimplifiedPipelineBuilder:
    """Builds simplified GStreamer pipeline with TCP output for local VLC testing."""
    
    def __init__(self, pipeline: Gst.Pipeline, config: StreamConfig):
        """Initialize simplified pipeline builder.
        
        Args:
            pipeline: Parent GStreamer pipeline
            config: Stream configuration
        """
        self.pipeline = pipeline
        self.config = config
        
        # Input elements
        self.offline_src = None
        self.srt_src = None
        
        # Demux elements
        self.offline_demux = None
        self.srt_demux = None
        
        # Queue elements (between demuxer and selector)
        self.offline_video_queue = None
        self.offline_audio_queue = None
        self.srt_video_queue = None
        self.srt_audio_queue = None
        
        # Switcher elements
        self.video_selector = None
        self.audio_selector = None
        self.offline_video_pad = None
        self.offline_audio_pad = None
        self.srt_video_pad = None
        self.srt_audio_pad = None
        
        # Output elements
        self.flvmux = None
        self.tcpserversink = None
        
        # Monitoring
        self.srt_data_callback = None
        self.verbose_probes = os.getenv('SRT_MONITOR_VERBOSE', 'false').lower() == 'true'
        self.probe_count = 0
        self.last_probe_log_time = time.time()
        
        print("[gst-simple] Simplified pipeline builder initialized", flush=True)
        if self.verbose_probes:
            print("[gst-simple] ⚙️  Verbose probe logging ENABLED", flush=True)
    
    def build(self, offline_fd: int, srt_fd: int) -> Tuple[Gst.Pad, Gst.Pad]:
        """Build the complete simplified pipeline with TCP output for VLC testing.
        
        Args:
            offline_fd: File descriptor for offline FFmpeg stdout
            srt_fd: File descriptor for SRT FFmpeg stdout
            
        Returns:
            Tuple of (offline_video_pad, srt_video_pad) for switching control
        """
        print("=" * 80, flush=True)
        print("🎬 V4.3 - TCP-ONLY PIPELINE (flvdemux → selectors → flvmux → tcpserversink) 🎬", flush=True)
        print("=" * 80, flush=True)
        print(f"[gst-simple] Building GStreamer pipeline with TCP output for VLC...", flush=True)
        
        # Create offline video input from FD
        self.offline_src = make_element("fdsrc", "offline-fdsrc")
        self.offline_src.set_property("fd", offline_fd)
        
        # Create SRT input from FD
        self.srt_src = make_element("fdsrc", "srt-fdsrc")
        self.srt_src.set_property("fd", srt_fd)
        
        # Create flvdemux for offline stream
        self.offline_demux = make_element("flvdemux", "offline-demux")
        
        # Create flvdemux for SRT stream
        self.srt_demux = make_element("flvdemux", "srt-demux")
        
        # Create video selector
        self.video_selector = make_element("input-selector", "video-selector")
        self.video_selector.set_property("sync-streams", False)
        self.video_selector.set_property("cache-buffers", False)
        
        # Create audio selector
        self.audio_selector = make_element("input-selector", "audio-selector")
        self.audio_selector.set_property("sync-streams", False)
        self.audio_selector.set_property("cache-buffers", False)
        
        # Create queues between demuxers and selectors (CRITICAL for smooth switching)
        # Offline video queue
        self.offline_video_queue = make_element("queue", "offline-video-queue")
        self.offline_video_queue.set_property("max-size-buffers", self.config.queue_buffer_size)
        self.offline_video_queue.set_property("max-size-time", 0)
        self.offline_video_queue.set_property("max-size-bytes", 0)
        self.offline_video_queue.set_property("leaky", 2)  # downstream - drop old buffers
        
        # Offline audio queue
        self.offline_audio_queue = make_element("queue", "offline-audio-queue")
        self.offline_audio_queue.set_property("max-size-buffers", self.config.queue_buffer_size)
        self.offline_audio_queue.set_property("max-size-time", 0)
        self.offline_audio_queue.set_property("max-size-bytes", 0)
        self.offline_audio_queue.set_property("leaky", 2)
        
        # SRT video queue
        self.srt_video_queue = make_element("queue", "srt-video-queue")
        self.srt_video_queue.set_property("max-size-buffers", 2)  # Minimal buffering for SRT
        self.srt_video_queue.set_property("max-size-time", 0)
        self.srt_video_queue.set_property("max-size-bytes", 0)
        self.srt_video_queue.set_property("leaky", 2)
        
        # SRT audio queue
        self.srt_audio_queue = make_element("queue", "srt-audio-queue")
        self.srt_audio_queue.set_property("max-size-buffers", 2)  # Minimal buffering for SRT
        self.srt_audio_queue.set_property("max-size-time", 0)
        self.srt_audio_queue.set_property("max-size-bytes", 0)
        self.srt_audio_queue.set_property("leaky", 2)
        
        # Create flvmux for output
        self.flvmux = make_element("flvmux", "output-mux")
        self.flvmux.set_property("streamable", True)
        self.flvmux.set_property("latency", 0)  # Minimize latency
        
        # Create TCP server sink for local VLC testing (always active)
        self.tcpserversink = make_element("tcpserversink", "tcp-sink")
        self.tcpserversink.set_property("host", "0.0.0.0")
        self.tcpserversink.set_property("port", self.config.tcp_port)
        self.tcpserversink.set_property("sync", False)
        self.tcpserversink.set_property("async", False)
        
        # Create queue elements with proper settings
        video_queue = make_element("queue", "video-queue")
        video_queue.set_property("max-size-buffers", 10)
        video_queue.set_property("max-size-time", 0)
        video_queue.set_property("max-size-bytes", 0)
        
        audio_queue = make_element("queue", "audio-queue")
        audio_queue.set_property("max-size-buffers", 10)
        audio_queue.set_property("max-size-time", 0)
        audio_queue.set_property("max-size-bytes", 0)
        
        # Create output queue
        output_queue = make_element("queue", "output-queue")
        output_queue.set_property("max-size-buffers", 10)
        output_queue.set_property("max-size-time", 0)
        output_queue.set_property("max-size-bytes", 0)
        
        # Add all elements to pipeline
        for elem in [self.offline_src, self.srt_src, self.offline_demux, self.srt_demux,
                     self.offline_video_queue, self.offline_audio_queue,
                     self.srt_video_queue, self.srt_audio_queue,
                     self.video_selector, self.audio_selector, video_queue, audio_queue,
                     self.flvmux, output_queue, self.tcpserversink]:
            self.pipeline.add(elem)
        
        # Link sources to demuxers
        if not self.offline_src.link(self.offline_demux):
            raise RuntimeError("Failed to link offline-fdsrc to offline-demux")
        if not self.srt_src.link(self.srt_demux):
            raise RuntimeError("Failed to link srt-fdsrc to srt-demux")
        
        # Connect demuxer pad-added signals
        self.offline_demux.connect("pad-added", self._on_offline_pad_added)
        self.srt_demux.connect("pad-added", self._on_srt_pad_added)
        
        # Link selectors to queues to flvmux (CRITICAL: correct order)
        # Video path: selector → queue → flvmux (video pad)
        if not self.video_selector.link(video_queue):
            raise RuntimeError("Failed to link video-selector to video-queue")
        
        # Audio path: selector → queue → flvmux (audio pad)
        if not self.audio_selector.link(audio_queue):
            raise RuntimeError("Failed to link audio-selector to audio-queue")
        
        # Request video pad from flvmux and link
        video_mux_pad = self.flvmux.request_pad_simple("video")
        if not video_mux_pad:
            raise RuntimeError("Failed to request video pad from flvmux")
        video_queue_src = video_queue.get_static_pad("src")
        if video_queue_src.link(video_mux_pad) != Gst.PadLinkReturn.OK:
            raise RuntimeError("Failed to link video-queue to flvmux video pad")
        
        # Request audio pad from flvmux and link
        audio_mux_pad = self.flvmux.request_pad_simple("audio")
        if not audio_mux_pad:
            raise RuntimeError("Failed to request audio pad from flvmux")
        audio_queue_src = audio_queue.get_static_pad("src")
        if audio_queue_src.link(audio_mux_pad) != Gst.PadLinkReturn.OK:
            raise RuntimeError("Failed to link audio-queue to flvmux audio pad")
        
        # Link flvmux to output_queue
        if not self.flvmux.link(output_queue):
            raise RuntimeError("Failed to link flvmux to output_queue")
        
        # Link output_queue to tee
        if not output_queue.link(self.tcpserversink):
            raise RuntimeError("Failed to link output_queue to tcpserversink")
        
        print(f"[gst-simple] ✓ Offline FD {offline_fd} configured", flush=True)
        print(f"[gst-simple] ✓ SRT FD {srt_fd} configured", flush=True)
        print("[gst-simple] ✓ Pipeline built with correct linking order:", flush=True)
        print("[gst-simple]   - fdsrc → flvdemux → queues → selectors → queues → flvmux → output-queue → tcpserversink", flush=True)
        print("[gst-simple]   - Queues between demux→selector prevent buffer blocking on inactive pad", flush=True)
        print("[gst-simple]   - SRT queues limited to 2 buffers to prevent accumulation delays", flush=True)
        print(f"[gst-simple] ✓ TCP server listening on port {self.config.tcp_port} for VLC", flush=True)
        print("[gst-simple] Waiting for flvdemux to add video/audio pads...", flush=True)
        
        # Return video pads for switching (will be populated after demux)
        return None, None  # Pads will be set in callbacks
    
    def _on_offline_pad_added(self, demux: Gst.Element, pad: Gst.Pad) -> None:
        """Handle new pad from offline demuxer with robust capability-based detection."""
        pad_name = pad.get_name()
        print(f"[gst-simple] 🔍 Offline demux pad added: '{pad_name}'", flush=True)
        
        # Get pad capabilities for robust detection
        caps = pad.query_caps(None)
        if caps and caps.get_size() > 0:
            structure = caps.get_structure(0)
            caps_name = structure.get_name()
            print(f"[gst-simple]   Pad caps: {caps_name}", flush=True)
            
            # Check if this is a video pad
            if caps_name.startswith("video/"):
                print(f"[gst-simple]   ✓ Detected VIDEO pad (caps: {caps_name})", flush=True)
                
                # Link demux → queue
                queue_sink = self.offline_video_queue.get_static_pad("sink")
                if pad.link(queue_sink) != Gst.PadLinkReturn.OK:
                    print(f"[gst-simple]   ❌ ERROR: Failed to link demux to queue", flush=True)
                    return
                
                # Request a sink pad from video selector
                self.offline_video_pad = self.video_selector.request_pad_simple("sink_%u")
                if not self.offline_video_pad:
                    print(f"[gst-simple]   ❌ ERROR: Failed to request video sink pad from selector", flush=True)
                    return
                
                print(f"[gst-simple]   Requested selector pad: {self.offline_video_pad.get_name()}", flush=True)
                
                # Link queue → selector
                queue_src = self.offline_video_queue.get_static_pad("src")
                link_result = queue_src.link(self.offline_video_pad)
                if link_result == Gst.PadLinkReturn.OK:
                    print(f"[gst-simple]   ✅ Offline VIDEO linked: {pad_name} → queue → {self.offline_video_pad.get_name()}", flush=True)
                else:
                    print(f"[gst-simple]   ❌ ERROR: Queue to selector link failed: {link_result.value_nick}", flush=True)
                    
            # Check if this is an audio pad
            elif caps_name.startswith("audio/"):
                print(f"[gst-simple]   ✓ Detected AUDIO pad (caps: {caps_name})", flush=True)
                
                # Link demux → queue
                queue_sink = self.offline_audio_queue.get_static_pad("sink")
                if pad.link(queue_sink) != Gst.PadLinkReturn.OK:
                    print(f"[gst-simple]   ❌ ERROR: Failed to link demux to queue", flush=True)
                    return
                
                # Request a sink pad from audio selector
                self.offline_audio_pad = self.audio_selector.request_pad_simple("sink_%u")
                if not self.offline_audio_pad:
                    print(f"[gst-simple]   ❌ ERROR: Failed to request audio sink pad from selector", flush=True)
                    return
                
                print(f"[gst-simple]   Requested selector pad: {self.offline_audio_pad.get_name()}", flush=True)
                
                # Link queue → selector
                queue_src = self.offline_audio_queue.get_static_pad("src")
                link_result = queue_src.link(self.offline_audio_pad)
                if link_result == Gst.PadLinkReturn.OK:
                    print(f"[gst-simple]   ✅ Offline AUDIO linked: {pad_name} → queue → {self.offline_audio_pad.get_name()}", flush=True)
                else:
                    print(f"[gst-simple]   ❌ ERROR: Queue to selector link failed: {link_result.value_nick}", flush=True)
            else:
                print(f"[gst-simple]   ⚠️  Unknown pad type: {caps_name}", flush=True)
        else:
            print(f"[gst-simple]   ⚠️  WARNING: Could not query pad capabilities", flush=True)
            print(f"[gst-simple]   Falling back to name-based detection for pad: {pad_name}", flush=True)
            
            # Fallback to name-based detection if caps query fails
            if "video" in pad_name.lower():
                queue_sink = self.offline_video_queue.get_static_pad("sink")
                pad.link(queue_sink)
                self.offline_video_pad = self.video_selector.request_pad_simple("sink_%u")
                if self.offline_video_pad:
                    queue_src = self.offline_video_queue.get_static_pad("src")
                    link_result = queue_src.link(self.offline_video_pad)
                    print(f"[gst-simple]   Video link result (fallback): {link_result.value_nick}", flush=True)
            elif "audio" in pad_name.lower():
                queue_sink = self.offline_audio_queue.get_static_pad("sink")
                pad.link(queue_sink)
                self.offline_audio_pad = self.audio_selector.request_pad_simple("sink_%u")
                if self.offline_audio_pad:
                    queue_src = self.offline_audio_queue.get_static_pad("src")
                    link_result = queue_src.link(self.offline_audio_pad)
                    print(f"[gst-simple]   Audio link result (fallback): {link_result.value_nick}", flush=True)
    
    def _on_srt_pad_added(self, demux: Gst.Element, pad: Gst.Pad) -> None:
        """Handle new pad from SRT demuxer with robust capability-based detection."""
        pad_name = pad.get_name()
        print(f"[gst-simple] 🔍 SRT demux pad added: '{pad_name}'", flush=True)
        
        # Get pad capabilities for robust detection
        caps = pad.query_caps(None)
        if caps and caps.get_size() > 0:
            structure = caps.get_structure(0)
            caps_name = structure.get_name()
            print(f"[gst-simple]   Pad caps: {caps_name}", flush=True)
            
            # Check if this is a video pad
            if caps_name.startswith("video/"):
                print(f"[gst-simple]   ✓ Detected VIDEO pad (caps: {caps_name})", flush=True)
                
                # Add probe on demuxed pad BEFORE linking to queue
                if self.srt_data_callback:
                    pad.add_probe(
                        Gst.PadProbeType.BUFFER,
                        self._on_srt_video_buffer_probe
                    )
                    print(f"[gst-simple]   ✅ Video probe attached to demuxed pad", flush=True)
                
                # Link demux → queue
                queue_sink = self.srt_video_queue.get_static_pad("sink")
                if pad.link(queue_sink) != Gst.PadLinkReturn.OK:
                    print(f"[gst-simple]   ❌ ERROR: Failed to link demux to queue", flush=True)
                    return
                
                # Request a sink pad from video selector
                self.srt_video_pad = self.video_selector.request_pad_simple("sink_%u")
                if not self.srt_video_pad:
                    print(f"[gst-simple]   ❌ ERROR: Failed to request video sink pad from selector", flush=True)
                    return
                
                print(f"[gst-simple]   Requested selector pad: {self.srt_video_pad.get_name()}", flush=True)
                
                # Link queue → selector
                queue_src = self.srt_video_queue.get_static_pad("src")
                link_result = queue_src.link(self.srt_video_pad)
                if link_result == Gst.PadLinkReturn.OK:
                    print(f"[gst-simple]   ✅ SRT VIDEO linked: {pad_name} → queue → {self.srt_video_pad.get_name()}", flush=True)
                else:
                    print(f"[gst-simple]   ❌ ERROR: Queue to selector link failed: {link_result.value_nick}", flush=True)
                    
            # Check if this is an audio pad
            elif caps_name.startswith("audio/"):
                print(f"[gst-simple]   ✓ Detected AUDIO pad (caps: {caps_name})", flush=True)
                
                # Add probe on demuxed pad BEFORE linking to queue
                if self.srt_data_callback:
                    pad.add_probe(
                        Gst.PadProbeType.BUFFER,
                        self._on_srt_audio_buffer_probe
                    )
                    print(f"[gst-simple]   ✅ Audio probe attached to demuxed pad", flush=True)
                
                # Link demux → queue
                queue_sink = self.srt_audio_queue.get_static_pad("sink")
                if pad.link(queue_sink) != Gst.PadLinkReturn.OK:
                    print(f"[gst-simple]   ❌ ERROR: Failed to link demux to queue", flush=True)
                    return
                
                # Request a sink pad from audio selector
                self.srt_audio_pad = self.audio_selector.request_pad_simple("sink_%u")
                if not self.srt_audio_pad:
                    print(f"[gst-simple]   ❌ ERROR: Failed to request audio sink pad from selector", flush=True)
                    return
                
                print(f"[gst-simple]   Requested selector pad: {self.srt_audio_pad.get_name()}", flush=True)
                
                # Link queue → selector
                queue_src = self.srt_audio_queue.get_static_pad("src")
                link_result = queue_src.link(self.srt_audio_pad)
                if link_result == Gst.PadLinkReturn.OK:
                    print(f"[gst-simple]   ✅ SRT AUDIO linked: {pad_name} → queue → {self.srt_audio_pad.get_name()}", flush=True)
                else:
                    print(f"[gst-simple]   ❌ ERROR: Queue to selector link failed: {link_result.value_nick}", flush=True)
            else:
                print(f"[gst-simple]   ⚠️  Unknown pad type: {caps_name}", flush=True)
        else:
            print(f"[gst-simple]   ⚠️  WARNING: Could not query pad capabilities", flush=True)
            print(f"[gst-simple]   Falling back to name-based detection for pad: {pad_name}", flush=True)
            
            # Fallback to name-based detection if caps query fails
            if "video" in pad_name.lower():
                if self.srt_data_callback:
                    pad.add_probe(Gst.PadProbeType.BUFFER, self._on_srt_video_buffer_probe)
                queue_sink = self.srt_video_queue.get_static_pad("sink")
                pad.link(queue_sink)
                self.srt_video_pad = self.video_selector.request_pad_simple("sink_%u")
                if self.srt_video_pad:
                    queue_src = self.srt_video_queue.get_static_pad("src")
                    link_result = queue_src.link(self.srt_video_pad)
                    print(f"[gst-simple]   Video link result (fallback): {link_result.value_nick}", flush=True)
            elif "audio" in pad_name.lower():
                if self.srt_data_callback:
                    pad.add_probe(Gst.PadProbeType.BUFFER, self._on_srt_audio_buffer_probe)
                queue_sink = self.srt_audio_queue.get_static_pad("sink")
                pad.link(queue_sink)
                self.srt_audio_pad = self.audio_selector.request_pad_simple("sink_%u")
                if self.srt_audio_pad:
                    queue_src = self.srt_audio_queue.get_static_pad("src")
                    link_result = queue_src.link(self.srt_audio_pad)
                    print(f"[gst-simple]   Audio link result (fallback): {link_result.value_nick}", flush=True)

    def get_video_selector(self) -> Gst.Element:
        """Get the video input selector element."""
        return self.video_selector
    
    def get_audio_selector(self) -> Gst.Element:
        """Get the audio input selector element."""
        return self.audio_selector
    
    def get_tcpserversink(self) -> Gst.Element:
        """Get the tcpserversink element for local VLC testing."""
        return self.tcpserversink
    
    def get_offline_pads(self) -> Tuple[Optional[Gst.Pad], Optional[Gst.Pad]]:
        """Get offline video and audio selector pads."""
        return self.offline_video_pad, self.offline_audio_pad
    
    def get_srt_pads(self) -> Tuple[Optional[Gst.Pad], Optional[Gst.Pad]]:
        """Get SRT video and audio selector pads."""
        return self.srt_video_pad, self.srt_audio_pad
    
    def _on_srt_video_buffer_probe(self, pad: Gst.Pad, info: Gst.PadProbeInfo) -> Gst.PadProbeReturn:
        """Probe callback for SRT video buffers."""
        if self.srt_data_callback:
            self.srt_data_callback()
        
        # Verbose logging for debugging
        if self.verbose_probes:
            self.probe_count += 1
            current_time = time.time()
            elapsed = current_time - self.last_probe_log_time
            
            # Log every second
            if elapsed >= 1.0:
                fps = self.probe_count / elapsed
                print(f"[probe] Video buffers: {self.probe_count} ({fps:.2f} fps)", flush=True)
                self.probe_count = 0
                self.last_probe_log_time = current_time
        
        return Gst.PadProbeReturn.OK
    
    def _on_srt_audio_buffer_probe(self, pad: Gst.Pad, info: Gst.PadProbeInfo) -> Gst.PadProbeReturn:
        """Probe callback for SRT audio buffers (secondary detection)."""
        if self.srt_data_callback:
            self.srt_data_callback()
        
        # Verbose logging (less frequent than video)
        if self.verbose_probes:
            current_time = time.time()
            elapsed = current_time - self.last_probe_log_time
            if elapsed >= 5.0:  # Log audio every 5 seconds
                print(f"[probe] Audio buffer detected (secondary detection active)", flush=True)
        
        return Gst.PadProbeReturn.OK
    
    def add_data_probe(self, pad_type: str, callback: Callable) -> None:
        """Store the data callback for probe attachment when pads are created.
        
        Note: The actual probes are now attached in _on_srt_pad_added() on the
        demuxed pads, not on the raw fdsrc. This provides much more reliable
        detection as we probe the decoded 30fps video stream.
        
        Args:
            pad_type: 'offline' or 'srt'
            callback: Callback function when data flows
        """
        if pad_type == "srt":
            self.srt_data_callback = callback
            print(f"[gst-simple] ✓ SRT data callback registered (will attach to demuxed pads)", flush=True)
        elif pad_type == "offline":
            # Offline monitoring not needed (always running)
            print(f"[gst-simple] ℹ️  Offline probe not needed (continuous loop)", flush=True)
        else:
            raise ValueError(f"Invalid pad_type: {pad_type}")