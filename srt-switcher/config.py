"""Configuration management for SRT Stream Switcher."""

import os
from dataclasses import dataclass
from typing import Optional


@dataclass
class StreamConfig:
    """Configuration for the SRT Stream Switcher.
    
    All settings are loaded from environment variables with sensible defaults.
    """
    
    # SRT settings
    srt_port: int
    srt_timeout: float
    
    # Video settings
    fallback_video: str
    output_width: int
    output_height: int
    output_fps: int
    output_bitrate: int
    
    # FFmpeg encoding settings
    ffmpeg_preset: str  # veryfast, faster, fast, medium, slow
    ffmpeg_tune: str    # zerolatency, film, animation, etc.
    ffmpeg_audio_bitrate: int  # Audio bitrate in kbps
    
    # Output settings
    output_url: str
    
    # TCP Server settings
    tcp_port: int
    
    # API settings
    api_port: int
    
    # Low-latency optimization settings (v5.0)
    low_latency_mode: bool
    srt_monitor_interval: float
    queue_buffer_size: int
    gop_duration: float
    
    @classmethod
    def from_environment(cls) -> 'StreamConfig':
        """Load configuration from environment variables."""
        kick_url = os.getenv('KICK_URL', '')
        kick_key = os.getenv('KICK_KEY', '')
        
        # Build output URL from Kick credentials or use default
        if kick_url and kick_key:
            output_url = f"{kick_url}/{kick_key}"
        else:
            output_url = os.getenv('OUTPUT_URL', 'rtmp://localhost/live/output')
        
        return cls(
            srt_port=int(os.getenv('SRT_PORT', '9000')),
            srt_timeout=float(os.getenv('SRT_TIMEOUT', '2.5')),
            fallback_video=os.getenv('FALLBACK_VIDEO', '/videos/fallback.mp4'),
            output_width=int(os.getenv('OUTPUT_WIDTH', '1920')),
            output_height=int(os.getenv('OUTPUT_HEIGHT', '1080')),
            output_fps=int(os.getenv('OUTPUT_FPS', '30')),
            output_bitrate=int(os.getenv('OUTPUT_BITRATE', '3000')),
            ffmpeg_preset=os.getenv('FFMPEG_PRESET', 'veryfast'),
            ffmpeg_tune=os.getenv('FFMPEG_TUNE', 'zerolatency'),
            ffmpeg_audio_bitrate=int(os.getenv('FFMPEG_AUDIO_BITRATE', '128')),
            output_url=output_url,
            tcp_port=int(os.getenv('TCP_PORT', '8554')),
            api_port=int(os.getenv('API_PORT', '8088')),
            # Low-latency optimization settings
            low_latency_mode=os.getenv('LOW_LATENCY_MODE', 'true').lower() == 'true',
            srt_monitor_interval=float(os.getenv('SRT_MONITOR_INTERVAL', '0.5')),
            queue_buffer_size=int(os.getenv('QUEUE_BUFFER_SIZE', '3')),
            gop_duration=float(os.getenv('GOP_DURATION', '0.5'))
        )
    
    def validate(self) -> None:
        """Validate configuration values."""
        if not os.path.exists(self.fallback_video):
            raise RuntimeError(f"Fallback video not found: {self.fallback_video}")
        
        # Check if file is readable
        if not os.access(self.fallback_video, os.R_OK):
            raise RuntimeError(f"Fallback video is not readable: {self.fallback_video}")
        
        # Check file size (should be at least 1KB)
        file_size = os.path.getsize(self.fallback_video)
        if file_size < 1024:
            raise RuntimeError(f"Fallback video file is too small ({file_size} bytes): {self.fallback_video}")
        
        print(f"[config] ✓ Fallback video file validated ({file_size} bytes)", flush=True)
        
        if self.srt_port <= 0 or self.srt_port > 65535:
            raise ValueError(f"Invalid SRT port: {self.srt_port}")
        
        if self.output_width <= 0 or self.output_height <= 0:
            raise ValueError(f"Invalid dimensions: {self.output_width}x{self.output_height}")
        
        if self.output_fps <= 0:
            raise ValueError(f"Invalid FPS: {self.output_fps}")
        
        # Validate FFmpeg preset
        valid_presets = ['ultrafast', 'superfast', 'veryfast', 'faster', 'fast', 'medium', 'slow', 'slower', 'veryslow']
        if self.ffmpeg_preset not in valid_presets:
            print(f"[config] Warning: Unknown FFmpeg preset '{self.ffmpeg_preset}', using 'veryfast'", flush=True)
            self.ffmpeg_preset = 'veryfast'
    
    def print_config(self) -> None:
        """Print configuration to console."""
        print(f"[config] SRT Port: {self.srt_port}", flush=True)
        print(f"[config] SRT Timeout: {self.srt_timeout}s", flush=True)
        print(f"[config] Fallback Video: {self.fallback_video}", flush=True)
        print(f"[config] Output URL: {self.output_url[:50]}...", flush=True)
        print(f"[config] Video: {self.output_bitrate}kbps, "
              f"{self.output_width}x{self.output_height}@{self.output_fps}fps", flush=True)
        print(f"[config] FFmpeg: preset={self.ffmpeg_preset}, tune={self.ffmpeg_tune}, "
              f"audio={self.ffmpeg_audio_bitrate}kbps", flush=True)
        print(f"[config] Low-latency: mode={self.low_latency_mode}, "
              f"monitor={self.srt_monitor_interval}s, queue={self.queue_buffer_size}, "
              f"gop={self.gop_duration}s", flush=True)