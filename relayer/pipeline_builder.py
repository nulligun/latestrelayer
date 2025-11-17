#!/usr/bin/env python3
"""
Pipeline Builder for Compositor
Handles GStreamer pipeline construction for fallback sources and output stage.
"""
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst

from config import (
    VIDEO_WIDTH,
    VIDEO_HEIGHT,
    VIDEO_FRAMERATE,
    AUDIO_RATE,
    AUDIO_CHANNELS,
    X264_PRESET,
    X264_BITRATE,
    FALLBACK_X264_BITRATE,
    AAC_BITRATE,
    OUTPUT_TCP_PORT,
)


class PipelineBuilder:
    """Builds GStreamer pipeline components."""
    
    def __init__(self, pipeline):
        """
        Initialize pipeline builder.
        
        Args:
            pipeline: GStreamer Pipeline object
        """
        self.pipeline = pipeline
        self.fallback_elements = {}
        self.output_elements = {}
        
        # Pad references for input-selectors
        self.video_selector_black_pad = None
        self.audio_selector_silence_pad = None
    
    def build_fallback_sources(self):
        """Create always-running fallback sources (black + silence) - pre-encoded once at startup for remuxing."""
        print("[build] Creating pre-encoded fallback sources (black H.264 + silence AAC)...", flush=True)
        
        # Black video source - pre-encode to H.264 at startup
        black_src = Gst.ElementFactory.make("videotestsrc", "black_src")
        black_src.set_property("pattern", 2)  # black
        black_src.set_property("is-live", True)
        black_src.set_property("do-timestamp", True)
        
        black_capsfilter = Gst.ElementFactory.make("capsfilter", "black_caps")
        black_caps = Gst.Caps.from_string(
            f"video/x-raw,width={VIDEO_WIDTH},height={VIDEO_HEIGHT},framerate={VIDEO_FRAMERATE}/1"
        )
        black_capsfilter.set_property("caps", black_caps)
        
        black_vconv = Gst.ElementFactory.make("videoconvert", "black_vconv")
        
        # Encode to H.264 once at startup (low bitrate since it's just black)
        black_x264 = Gst.ElementFactory.make("x264enc", "black_x264")
        black_x264.set_property("tune", "zerolatency")
        black_x264.set_property("speed-preset", "ultrafast")
        black_x264.set_property("bitrate", FALLBACK_X264_BITRATE)  # 500kbps
        black_x264.set_property("key-int-max", 30)
        
        black_h264parse = Gst.ElementFactory.make("h264parse", "black_h264parse")
        black_queue = Gst.ElementFactory.make("queue", "black_queue")
        
        # Silent audio source - pre-encode to AAC at startup
        silence_src = Gst.ElementFactory.make("audiotestsrc", "silence_src")
        silence_src.set_property("wave", 4)  # silence
        silence_src.set_property("is-live", True)
        silence_src.set_property("do-timestamp", True)
        silence_src.set_property("volume", 0.0)
        
        silence_capsfilter = Gst.ElementFactory.make("capsfilter", "silence_caps")
        silence_caps = Gst.Caps.from_string(
            f"audio/x-raw,rate={AUDIO_RATE},channels={AUDIO_CHANNELS}"
        )
        silence_capsfilter.set_property("caps", silence_caps)
        
        silence_aconv = Gst.ElementFactory.make("audioconvert", "silence_aconv")
        silence_ares = Gst.ElementFactory.make("audioresample", "silence_ares")
        
        # Encode to AAC once at startup
        silence_aacenc = Gst.ElementFactory.make("avenc_aac", "silence_aac")
        silence_aacenc.set_property("bitrate", AAC_BITRATE)
        
        silence_aacparse = Gst.ElementFactory.make("aacparse", "silence_aacparse")
        silence_queue = Gst.ElementFactory.make("queue", "silence_queue")
        
        self.fallback_elements = {
            'black_src': black_src,
            'black_capsfilter': black_capsfilter,
            'black_vconv': black_vconv,
            'black_x264': black_x264,
            'black_h264parse': black_h264parse,
            'black_queue': black_queue,
            'silence_src': silence_src,
            'silence_capsfilter': silence_capsfilter,
            'silence_aconv': silence_aconv,
            'silence_ares': silence_ares,
            'silence_aacenc': silence_aacenc,
            'silence_aacparse': silence_aacparse,
            'silence_queue': silence_queue,
        }
        
        # Add to pipeline
        for elem in self.fallback_elements.values():
            self.pipeline.add(elem)
        
        # Link fallback video chain: src → caps → convert → encode → parse → queue
        black_src.link(black_capsfilter)
        black_capsfilter.link(black_vconv)
        black_vconv.link(black_x264)
        black_x264.link(black_h264parse)
        black_h264parse.link(black_queue)
        
        # Link fallback audio chain: src → caps → convert → resample → encode → parse → queue
        silence_src.link(silence_capsfilter)
        silence_capsfilter.link(silence_aconv)
        silence_aconv.link(silence_ares)
        silence_ares.link(silence_aacenc)
        silence_aacenc.link(silence_aacparse)
        silence_aacparse.link(silence_queue)
        
        print(f"[build] ✓ Fallback sources created (H.264 @ {FALLBACK_X264_BITRATE}kbps, AAC @ {AAC_BITRATE}bps)", flush=True)
        return self.fallback_elements
    
    def build_output_stage(self):
        """Create always-running output stage (input-selectors + mux + TCP) - NO ENCODING (remux only)."""
        print("[build] Creating shared output stage with input-selectors (remux-only, no encoding)...", flush=True)
        
        # Video input-selector (now expects H.264 input)
        video_selector = Gst.ElementFactory.make("input-selector", "video_sel")
        video_selector.set_property("sync-streams", True)  # Critical: maintains continuous output
        video_selector.set_property("cache-buffers", True)  # Reduces latency
        
        # Audio input-selector (now expects AAC input)
        audio_selector = Gst.ElementFactory.make("input-selector", "audio_sel")
        audio_selector.set_property("sync-streams", True)  # Critical: maintains continuous output
        audio_selector.set_property("cache-buffers", True)  # Reduces latency
        
        # Queue before muxing (no encoding needed - all sources are pre-encoded)
        video_mux_queue = Gst.ElementFactory.make("queue", "video_mux_q")
        audio_mux_queue = Gst.ElementFactory.make("queue", "audio_mux_q")
        
        # Muxer and output
        mpegtsmux = Gst.ElementFactory.make("mpegtsmux", "mux")
        mpegtsmux.set_property("alignment", 7)
        
        tcp_sink = Gst.ElementFactory.make("tcpserversink", "tcp_sink")
        tcp_sink.set_property("host", "0.0.0.0")
        tcp_sink.set_property("port", OUTPUT_TCP_PORT)
        tcp_sink.set_property("sync-method", 0)
        tcp_sink.set_property("recover-policy", 0)
        
        self.output_elements = {
            'video_selector': video_selector,
            'audio_selector': audio_selector,
            'video_mux_queue': video_mux_queue,
            'audio_mux_queue': audio_mux_queue,
            'mpegtsmux': mpegtsmux,
            'tcp_sink': tcp_sink,
        }
        
        # Add to pipeline
        for elem in self.output_elements.values():
            self.pipeline.add(elem)
        
        # Link video path: selector → queue → mux (NO ENCODING!)
        video_selector.link(video_mux_queue)
        
        # Link audio path: selector → queue → mux (NO ENCODING!)
        audio_selector.link(audio_mux_queue)
        
        # Link queues to muxer
        video_mux_queue.link(mpegtsmux)
        audio_mux_queue.link(mpegtsmux)
        
        # Link muxer to TCP sink
        mpegtsmux.link(tcp_sink)
        
        print("[build] ✓ Output stage created (remux-only pipeline, NO re-encoding)", flush=True)
        return self.output_elements
    
    def link_fallback_to_selectors(self):
        """Link fallback sources to input-selectors as default pads."""
        print("[build] Linking fallback to input-selectors...", flush=True)
        
        video_selector = self.output_elements['video_selector']
        audio_selector = self.output_elements['audio_selector']
        
        # Log element states for debugging
        vret, vstate, vpending = video_selector.get_state(Gst.CLOCK_TIME_NONE)
        aret, astate, apending = audio_selector.get_state(Gst.CLOCK_TIME_NONE)
        print(f"[build] Selector states before pad creation: video={vstate.value_nick}, audio={astate.value_nick}", flush=True)
        
        # Link fallback video to selector
        black_queue = self.fallback_elements['black_queue']
        
        # Request video pad with validation
        self.video_selector_black_pad = video_selector.request_pad_simple("sink_%u")
        if not self.video_selector_black_pad:
            raise RuntimeError("Failed to create video selector sink pad for fallback")
        
        # Get and validate black queue src pad
        black_queue_src = black_queue.get_static_pad("src")
        if not black_queue_src:
            raise RuntimeError("Failed to get black_queue src pad")
        
        # Link and validate result
        link_result = black_queue_src.link(self.video_selector_black_pad)
        if link_result != Gst.PadLinkReturn.OK:
            raise RuntimeError(f"Failed to link fallback video to selector: {link_result}")
        print(f"[build] Fallback video → selector link: {link_result}", flush=True)
        
        # Set black pad as initially active
        video_selector.set_property("active-pad", self.video_selector_black_pad)
        print("[build] Set black screen as active video source", flush=True)
        
        # Link fallback audio to selector
        silence_queue = self.fallback_elements['silence_queue']
        
        # Request audio pad with validation
        self.audio_selector_silence_pad = audio_selector.request_pad_simple("sink_%u")
        if not self.audio_selector_silence_pad:
            raise RuntimeError("Failed to create audio selector sink pad for fallback")
        
        # Get and validate silence queue src pad
        silence_queue_src = silence_queue.get_static_pad("src")
        if not silence_queue_src:
            raise RuntimeError("Failed to get silence_queue src pad")
        
        # Link and validate result
        link_result = silence_queue_src.link(self.audio_selector_silence_pad)
        if link_result != Gst.PadLinkReturn.OK:
            raise RuntimeError(f"Failed to link fallback audio to selector: {link_result}")
        print(f"[build] Fallback audio → selector link: {link_result}", flush=True)
        
        # Set silence pad as initially active
        audio_selector.set_property("active-pad", self.audio_selector_silence_pad)
        print("[build] Set silence as active audio source", flush=True)
        
        print("[build] ✓ Fallback linked to selectors and activated", flush=True)
    
    def get_fallback_elements(self):
        """Get fallback elements dictionary."""
        return self.fallback_elements
    
    def get_output_elements(self):
        """Get output elements dictionary."""
        return self.output_elements
    
    def get_selector_pads(self):
        """Get the fallback selector pad references."""
        return {
            'video_black_pad': self.video_selector_black_pad,
            'audio_silence_pad': self.audio_selector_silence_pad,
        }