"""GStreamer pipeline builders for SRT Stream Switcher."""

import gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst

from typing import Callable, Optional, List
from config import StreamConfig


def make_element(factory: str, name: str) -> Gst.Element:
    """Create a GStreamer element with error checking.
    
    Args:
        factory: Element factory name (e.g., 'filesrc', 'x264enc')
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


class VideoProcessingChain:
    """Builds and manages a video processing chain.
    
    Handles: queue -> convert -> scale -> rate -> caps -> encode -> parse -> queue
    """
    
    def __init__(self, prefix: str, config: StreamConfig):
        """Initialize video processing chain.
        
        Args:
            prefix: Prefix for element names (e.g., 'offline', 'srt')
            config: Stream configuration
        """
        self.prefix = prefix
        self.config = config
        self.elements: List[Gst.Element] = []
        
        # Create elements
        self.input_queue = self._create_queue(f"{prefix}-vqueue1", leaky=True)
        self.convert = make_element("videoconvert", f"{prefix}-vconvert")
        self.scale = make_element("videoscale", f"{prefix}-vscale")
        self.rate = make_element("videorate", f"{prefix}-vrate")
        self.rate.set_property("drop-only", True)
        
        self.caps = make_element("capsfilter", f"{prefix}-vcaps")
        self.caps.set_property("caps", Gst.Caps.from_string(
            f"video/x-raw,format=I420,width={config.output_width},"
            f"height={config.output_height},framerate={config.output_fps}/1"
        ))
        
        self.encoder = make_element("x264enc", f"{prefix}-venc")
        self.encoder.set_property("bitrate", config.output_bitrate)
        self.encoder.set_property("tune", "zerolatency")
        self.encoder.set_property("speed-preset", "veryfast")
        self.encoder.set_property("key-int-max", config.output_fps * 2)
        
        self.parser = make_element("h264parse", f"{prefix}-vparse")
        self.output_queue = self._create_queue(f"{prefix}-vqueue2", unlimited=True)
        
        self.elements = [
            self.input_queue, self.convert, self.scale, self.rate,
            self.caps, self.encoder, self.parser, self.output_queue
        ]
    
    def _create_queue(self, name: str, leaky: bool = False, unlimited: bool = False) -> Gst.Element:
        """Create a queue with appropriate settings."""
        queue = make_element("queue", name)
        if leaky:
            queue.set_property("max-size-buffers", 30)
            queue.set_property("leaky", 1)
        elif unlimited:
            queue.set_property("max-size-buffers", 0)
            queue.set_property("max-size-time", 0)
            queue.set_property("max-size-bytes", 0)
        return queue
    
    def add_to_pipeline(self, pipeline: Gst.Pipeline) -> None:
        """Add all elements to the pipeline."""
        for elem in self.elements:
            pipeline.add(elem)
    
    def link(self) -> None:
        """Link all elements in the chain."""
        for i in range(len(self.elements) - 1):
            self.elements[i].link(self.elements[i + 1])
    
    def get_sink_pad(self) -> Gst.Pad:
        """Get the sink pad of the first element."""
        return self.input_queue.get_static_pad("sink")
    
    def get_src_pad(self) -> Gst.Pad:
        """Get the source pad of the last element."""
        return self.output_queue.get_static_pad("src")


class AudioProcessingChain:
    """Builds and manages an audio processing chain.
    
    Handles: queue -> convert -> resample -> caps -> encode -> parse -> queue
    """
    
    def __init__(self, prefix: str, config: StreamConfig):
        """Initialize audio processing chain.
        
        Args:
            prefix: Prefix for element names (e.g., 'offline', 'srt')
            config: Stream configuration
        """
        self.prefix = prefix
        self.config = config
        self.elements: List[Gst.Element] = []
        
        # Create elements
        self.input_queue = self._create_queue(f"{prefix}-aqueue1", leaky=True)
        self.convert = make_element("audioconvert", f"{prefix}-aconvert")
        self.resample = make_element("audioresample", f"{prefix}-aresample")
        
        self.caps = make_element("capsfilter", f"{prefix}-acaps")
        self.caps.set_property("caps", Gst.Caps.from_string(
            "audio/x-raw,channels=2,rate=48000"
        ))
        
        self.encoder = make_element("lamemp3enc", f"{prefix}-aenc")
        self.encoder.set_property("bitrate", 128)
        self.encoder.set_property("cbr", True)
        
        self.parser = make_element("mpegaudioparse", f"{prefix}-aparse")
        self.output_queue = self._create_queue(f"{prefix}-aqueue2", unlimited=True)
        
        self.elements = [
            self.input_queue, self.convert, self.resample,
            self.caps, self.encoder, self.parser, self.output_queue
        ]
    
    def _create_queue(self, name: str, leaky: bool = False, unlimited: bool = False) -> Gst.Element:
        """Create a queue with appropriate settings."""
        queue = make_element("queue", name)
        if leaky:
            queue.set_property("max-size-buffers", 30)
            queue.set_property("leaky", 1)
        elif unlimited:
            queue.set_property("max-size-buffers", 0)
            queue.set_property("max-size-time", 0)
            queue.set_property("max-size-bytes", 0)
        return queue
    
    def add_to_pipeline(self, pipeline: Gst.Pipeline) -> None:
        """Add all elements to the pipeline."""
        for elem in self.elements:
            pipeline.add(elem)
    
    def link(self) -> None:
        """Link all elements in the chain."""
        for i in range(len(self.elements) - 1):
            self.elements[i].link(self.elements[i + 1])
    
    def get_sink_pad(self) -> Gst.Pad:
        """Get the sink pad of the first element."""
        return self.input_queue.get_static_pad("sink")
    
    def get_src_pad(self) -> Gst.Pad:
        """Get the source pad of the last element."""
        return self.output_queue.get_static_pad("src")


class OfflinePipelineBuilder:
    """Builds the offline video looping pipeline."""
    
    def __init__(self, pipeline: Gst.Pipeline, config: StreamConfig):
        """Initialize offline pipeline builder.
        
        Args:
            pipeline: Parent GStreamer pipeline
            config: Stream configuration
        """
        self.pipeline = pipeline
        self.config = config
        self.bus = pipeline.get_bus()
        
        # Create source and decode
        self.filesrc = make_element("filesrc", "offline-src")
        self.filesrc.set_property("location", config.fallback_video)
        
        self.decodebin = make_element("decodebin", "offline-decode")
        
        # Create convert elements BEFORE queues to handle any format from decodebin
        # This is critical for non-interleaved audio which queues cannot accept
        # audioconvert can accept non-interleaved and naturally outputs interleaved
        self.video_convert = make_element("videoconvert", "offline-decode-vconv")
        self.audio_convert = make_element("audioconvert", "offline-decode-aconv")
        self.audio_resample = make_element("audioresample", "offline-decode-aresample")
        
        # Create queue elements to decouple decodebin from processing chains
        self.video_queue = make_element("queue", "offline-decode-vqueue")
        self.video_queue.set_property("max-size-buffers", 5)
        self.audio_queue = make_element("queue", "offline-decode-aqueue")
        self.audio_queue.set_property("max-size-buffers", 5)
        
        # Create processing chains
        self.video_chain = VideoProcessingChain("offline", config)
        self.audio_chain = AudioProcessingChain("offline", config)
        
        # Create muxer
        self.mux = make_element("flvmux", "offline-mux")
        self.mux.set_property("streamable", True)
        
        self.mux_identity = make_element("identity", "offline-mux-identity")
        self.mux_identity.set_property("sync", False)
        
        self.output_element = self.mux_identity
    
    def build(self) -> Gst.Element:
        """Build and link the offline pipeline.
        
        Returns:
            Output element to connect to selector
        """
        print(f"[offline] Building offline video pipeline from: {self.config.fallback_video}", flush=True)
        
        # Add all elements to pipeline
        self.pipeline.add(self.filesrc)
        self.pipeline.add(self.decodebin)
        self.pipeline.add(self.video_convert)
        self.pipeline.add(self.audio_convert)
        self.pipeline.add(self.audio_resample)
        self.pipeline.add(self.video_queue)
        self.pipeline.add(self.audio_queue)
        self.video_chain.add_to_pipeline(self.pipeline)
        self.audio_chain.add_to_pipeline(self.pipeline)
        self.pipeline.add(self.mux)
        self.pipeline.add(self.mux_identity)
        
        # Link source to decoder
        self.filesrc.link(self.decodebin)
        
        # Link convert -> resample -> queue -> processing chain
        # audioconvert accepts non-interleaved and outputs interleaved
        # Decodebin pads will be linked to convert in _on_pad_added
        self.video_convert.link(self.video_queue)
        self.video_queue.link(self.video_chain.convert)
        self.audio_convert.link(self.audio_resample)
        self.audio_resample.link(self.audio_queue)
        self.audio_queue.link(self.audio_chain.caps)
        
        # Link processing chains internally
        self.video_chain.link()
        self.audio_chain.link()
        
        # Connect to mux
        mux_video_pad = self.mux.request_pad_simple("video")
        mux_audio_pad = self.mux.request_pad_simple("audio")
        self.video_chain.get_src_pad().link(mux_video_pad)
        self.audio_chain.get_src_pad().link(mux_audio_pad)
        
        # Link mux output
        self.mux.link(self.mux_identity)
        
        # Connect decoder dynamic pads
        self.decodebin.connect("pad-added", self._on_pad_added)
        
        # Set up looping
        self.bus.connect("message::eos", self._on_eos)
        
        print("[offline] ✓ Offline video pipeline created", flush=True)
        return self.output_element
    
    def _on_pad_added(self, element: Gst.Element, pad: Gst.Pad) -> None:
        """Handle dynamic pads from decodebin."""
        caps = pad.get_current_caps()
        if not caps:
            print("[offline] Warning: Pad added but no caps available yet", flush=True)
            return
        
        struct = caps.get_structure(0)
        name = struct.get_name()
        
        # Log the caps being negotiated
        print(f"[offline] Decodebin pad added: {name}", flush=True)
        print(f"[offline]   Caps: {caps.to_string()}", flush=True)
        
        if name.startswith("video/"):
            sink = self.video_convert.get_static_pad("sink")
            if not sink.is_linked():
                result = pad.link(sink)
                if result == Gst.PadLinkReturn.OK:
                    print("[offline] ✓ Video pad linked to convert", flush=True)
                else:
                    print(f"[offline] ERROR: Video pad link failed: {result}", flush=True)
        elif name.startswith("audio/"):
            sink = self.audio_convert.get_static_pad("sink")
            if not sink.is_linked():
                result = pad.link(sink)
                if result == Gst.PadLinkReturn.OK:
                    print("[offline] ✓ Audio pad linked to convert", flush=True)
                else:
                    print(f"[offline] ERROR: Audio pad link failed: {result}", flush=True)
    
    def _on_eos(self, bus: Gst.Bus, msg: Gst.Message) -> None:
        """Handle end-of-stream for looping."""
        if msg.src == self.filesrc:
            print("[offline] ✓ End of file reached, seeking to start for loop", flush=True)
            self.filesrc.seek_simple(
                Gst.Format.TIME,
                Gst.SeekFlags.FLUSH | Gst.SeekFlags.KEY_UNIT,
                0
            )


class SRTPipelineBuilder:
    """Builds the SRT input pipeline."""
    
    def __init__(self, pipeline: Gst.Pipeline, config: StreamConfig, 
                 data_probe_callback: Optional[Callable] = None):
        """Initialize SRT pipeline builder.
        
        Args:
            pipeline: Parent GStreamer pipeline
            config: Stream configuration
            data_probe_callback: Callback for data flow monitoring
        """
        self.pipeline = pipeline
        self.config = config
        self.data_probe_callback = data_probe_callback
        
        # Create source and decode
        self.srtsrc = make_element("srtsrc", "srt-src")
        self.srtsrc.set_property("uri", f"srt://:{config.srt_port}?mode=listener")
        self.srtsrc.set_property("wait-for-connection", False)
        
        self.decodebin = make_element("decodebin", "srt-decode")
        self.decodebin.set_property("async-handling", True)
        
        # Create convert elements BEFORE queues to handle any format from decodebin
        # This is critical for non-interleaved audio which queues cannot accept
        self.video_convert = make_element("videoconvert", "srt-decode-vconv")
        self.audio_convert = make_element("audioconvert", "srt-decode-aconv")
        self.audio_resample = make_element("audioresample", "srt-decode-aresample")
        
        # Create queue elements to decouple decodebin from processing chains
        self.video_queue = make_element("queue", "srt-decode-vqueue")
        self.video_queue.set_property("max-size-buffers", 5)
        self.audio_queue = make_element("queue", "srt-decode-aqueue")
        self.audio_queue.set_property("max-size-buffers", 5)
        
        # Create processing chains
        self.video_chain = VideoProcessingChain("srt", config)
        self.audio_chain = AudioProcessingChain("srt", config)
        
        # Create muxer
        self.mux = make_element("flvmux", "srt-mux")
        self.mux.set_property("streamable", True)
        
        self.mux_identity = make_element("identity", "srt-mux-identity")
        self.mux_identity.set_property("sync", False)
        
        self.output_element = self.mux_identity
    
    def build(self) -> Gst.Element:
        """Build and link the SRT pipeline.
        
        Returns:
            Output element to connect to selector
        """
        print(f"[srt] Building SRT pipeline on port {self.config.srt_port}...", flush=True)
        
        # Add all elements to pipeline
        self.pipeline.add(self.srtsrc)
        self.pipeline.add(self.decodebin)
        self.pipeline.add(self.video_convert)
        self.pipeline.add(self.audio_convert)
        self.pipeline.add(self.audio_resample)
        self.pipeline.add(self.video_queue)
        self.pipeline.add(self.audio_queue)
        self.video_chain.add_to_pipeline(self.pipeline)
        self.audio_chain.add_to_pipeline(self.pipeline)
        self.pipeline.add(self.mux)
        self.pipeline.add(self.mux_identity)
        
        # Link source to decoder
        self.srtsrc.link(self.decodebin)
        
        # Link convert -> resample -> queue -> processing chain
        # audioconvert accepts non-interleaved and outputs interleaved
        # Decodebin pads will be linked to audioconvert in _on_pad_added
        # Link directly to processing chain's convert (not input_queue) to avoid queue-to-queue
        self.video_convert.link(self.video_queue)
        self.video_queue.link(self.video_chain.convert)
        self.audio_convert.link(self.audio_resample)
        self.audio_resample.link(self.audio_queue)
        self.audio_queue.link(self.audio_chain.caps)
        
        # Link processing chains internally
        self.video_chain.link()
        self.audio_chain.link()
        
        # Connect to mux
        mux_video_pad = self.mux.request_pad_simple("video")
        mux_audio_pad = self.mux.request_pad_simple("audio")
        self.video_chain.get_src_pad().link(mux_video_pad)
        self.audio_chain.get_src_pad().link(mux_audio_pad)
        
        # Link mux output
        self.mux.link(self.mux_identity)
        
        # Connect decoder dynamic pads
        self.decodebin.connect("pad-added", self._on_pad_added)
        
        # Add data probe if callback provided
        if self.data_probe_callback:
            identity_src = self.mux_identity.get_static_pad("src")
            identity_src.add_probe(
                Gst.PadProbeType.BUFFER,
                lambda pad, info: (self.data_probe_callback(), Gst.PadProbeReturn.OK)[1]
            )
        
        print("[srt] ✓ SRT pipeline created (waiting for connection)", flush=True)
        return self.output_element
    
    def _on_pad_added(self, element: Gst.Element, pad: Gst.Pad) -> None:
        """Handle dynamic pads from decodebin."""
        caps = pad.get_current_caps()
        if not caps:
            print("[srt] Warning: Pad added but no caps available yet", flush=True)
            return
        
        struct = caps.get_structure(0)
        name = struct.get_name()
        
        # Log the caps being negotiated
        print(f"[srt] Decodebin pad added: {name}", flush=True)
        print(f"[srt]   Caps: {caps.to_string()}", flush=True)
        
        if name.startswith("video/"):
            sink = self.video_convert.get_static_pad("sink")
            if not sink.is_linked():
                result = pad.link(sink)
                if result == Gst.PadLinkReturn.OK:
                    print("[srt] ✓ Video pad linked to convert - SRT stream connected!", flush=True)
                else:
                    print(f"[srt] ERROR: Video pad link failed: {result}", flush=True)
        elif name.startswith("audio/"):
            sink = self.audio_convert.get_static_pad("sink")
            if not sink.is_linked():
                result = pad.link(sink)
                if result == Gst.PadLinkReturn.OK:
                    print("[srt] ✓ Audio pad linked to convert", flush=True)
                else:
                    print(f"[srt] ERROR: Audio pad link failed: {result}", flush=True)


class OutputPipelineBuilder:
    """Builds the selector and output pipeline."""
    
    def __init__(self, pipeline: Gst.Pipeline, config: StreamConfig):
        """Initialize output pipeline builder.
        
        Args:
            pipeline: Parent GStreamer pipeline
            config: Stream configuration
        """
        self.pipeline = pipeline
        self.config = config
        
        # Create selector
        self.selector = make_element("input-selector", "flv-selector")
        self.selector.set_property("sync-streams", False)
        self.selector.set_property("cache-buffers", False)
        
        # Create output elements
        self.queue = make_element("queue", "output-queue")
        self.queue.set_property("max-size-buffers", 30)
        
        self.tee = make_element("tee", "output-tee")
        
        self.fakesink = make_element("fakesink", "output-fakesink")
        self.fakesink.set_property("sync", False)
        self.fakesink.set_property("async", False)
        
        # Create valve branch for Kick streaming
        self.valve = make_element("valve", "kick-valve")
        self.valve.set_property("drop", True)  # Start closed
        
        self.valve_queue = make_element("queue", "kick-queue")
        self.valve_queue.set_property("max-size-buffers", 30)
        
        self.rtmpsink = make_element("rtmpsink", "kick-sink")
        self.rtmpsink.set_property("location", config.output_url)
        self.rtmpsink.set_property("async", False)
    
    def build(self, offline_output: Gst.Element, srt_output: Gst.Element) -> tuple[Gst.Pad, Gst.Pad]:
        """Build and link the output pipeline.
        
        Args:
            offline_output: Output element from offline pipeline
            srt_output: Output element from SRT pipeline
            
        Returns:
            Tuple of (offline_pad, srt_pad) for switching
        """
        print("[output] Building selector and output pipeline...", flush=True)
        
        # Add all elements to pipeline
        for elem in [self.selector, self.queue, self.tee, self.fakesink,
                     self.valve, self.valve_queue, self.rtmpsink]:
            self.pipeline.add(elem)
        
        # Link output chain
        self.selector.link(self.queue)
        self.queue.link(self.tee)
        self.tee.link(self.fakesink)
        self.tee.link(self.valve)
        self.valve.link(self.valve_queue)
        self.valve_queue.link(self.rtmpsink)
        
        # Connect inputs to selector
        offline_src = offline_output.get_static_pad("src")
        srt_src = srt_output.get_static_pad("src")
        
        offline_pad = self.selector.request_pad_simple("sink_%u")
        srt_pad = self.selector.request_pad_simple("sink_%u")
        
        offline_src.link(offline_pad)
        srt_src.link(srt_pad)
        
        print(f"[output] ✓ Offline linked to selector pad: {offline_pad.get_name()}", flush=True)
        print(f"[output] ✓ SRT linked to selector pad: {srt_pad.get_name()}", flush=True)
        print("[output] ✓ Selector and output pipeline created", flush=True)
        
        return offline_pad, srt_pad