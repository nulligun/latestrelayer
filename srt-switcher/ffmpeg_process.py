"""FFmpeg process management for normalized stream inputs."""

import subprocess
import threading
import time
import sys
from typing import Optional, Callable
from datetime import datetime

from config import StreamConfig


class FFmpegProcess:
    """Manages a single FFmpeg subprocess and monitors its output."""
    
    def __init__(self, name: str, args: list, data_callback: Optional[Callable] = None, use_stdin: bool = False):
        """Initialize FFmpeg process.
        
        Args:
            name: Process name for logging
            args: FFmpeg command arguments
            data_callback: Callback when data flows through stdout
            use_stdin: If True, open stdin as pipe for input (used by output process)
        """
        self.name = name
        self.args = args
        self.data_callback = data_callback
        self.use_stdin = use_stdin
        
        self.process: Optional[subprocess.Popen] = None
        self.monitor_thread: Optional[threading.Thread] = None
        self.running = False
        self.last_data_time: Optional[float] = None
        
        print(f"[{self.name}] FFmpeg process initialized", flush=True)
    
    def start(self) -> bool:
        """Start the FFmpeg process.
        
        Returns:
            True if started successfully
        """
        if self.process and self.process.poll() is None:
            print(f"[{self.name}] Process already running", flush=True)
            return True
        
        try:
            print(f"[{self.name}] Starting FFmpeg: {' '.join(self.args[:5])}...", flush=True)
            
            # Configure stdin based on use_stdin flag
            stdin_config = subprocess.PIPE if self.use_stdin else None
            
            # Start FFmpeg with stdout as pipe for data (or stdin for output process)
            self.process = subprocess.Popen(
                self.args,
                stdin=stdin_config,
                stdout=subprocess.PIPE if not self.use_stdin else None,
                stderr=subprocess.PIPE,
                bufsize=0  # Unbuffered for real-time data
            )
            
            self.running = True
            
            # Start monitoring thread for stderr (logs)
            self.monitor_thread = threading.Thread(
                target=self._monitor_stderr,
                daemon=True
            )
            self.monitor_thread.start()
            
            print(f"[{self.name}] ✓ FFmpeg started (PID: {self.process.pid})", flush=True)
            return True
            
        except Exception as e:
            print(f"[{self.name}] ERROR: Failed to start FFmpeg: {e}", 
                  file=sys.stderr, flush=True)
            return False
    
    def stop(self) -> None:
        """Stop the FFmpeg process gracefully."""
        if not self.process:
            return
        
        print(f"[{self.name}] Stopping FFmpeg...", flush=True)
        self.running = False
        
        try:
            # Send SIGTERM for graceful shutdown
            self.process.terminate()
            
            # Wait up to 5 seconds for graceful shutdown
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                print(f"[{self.name}] Force killing FFmpeg", flush=True)
                self.process.kill()
                self.process.wait()
            
            print(f"[{self.name}] ✓ FFmpeg stopped", flush=True)
            
        except Exception as e:
            print(f"[{self.name}] Error stopping FFmpeg: {e}", 
                  file=sys.stderr, flush=True)
        
        self.process = None
    
    def is_running(self) -> bool:
        """Check if process is running.
        
        Returns:
            True if process is alive
        """
        return self.process is not None and self.process.poll() is None
    
    def get_stdout(self) -> Optional[int]:
        """Get the file descriptor for stdout pipe.
        
        Returns:
            File descriptor number or None
        """
        if self.process and self.process.stdout:
            return self.process.stdout.fileno()
        return None
    
    def get_stdin(self) -> Optional[int]:
        """Get the file descriptor for stdin pipe.
        
        Returns:
            File descriptor number or None
        """
        if self.process and self.process.stdin:
            return self.process.stdin.fileno()
        return None
    
    def update_data_time(self) -> None:
        """Update last data received time."""
        self.last_data_time = time.time()
        if self.data_callback:
            self.data_callback()
    
    def get_last_data_age(self) -> Optional[float]:
        """Get seconds since last data received.
        
        Returns:
            Seconds since last data, or None if no data yet
        """
        if self.last_data_time is None:
            return None
        return time.time() - self.last_data_time
    
    def _monitor_stderr(self) -> None:
        """Monitor FFmpeg stderr output in background thread."""
        if not self.process or not self.process.stderr:
            return
        
        print(f"[{self.name}] Started stderr monitor", flush=True)
        
        try:
            for line in iter(self.process.stderr.readline, b''):
                if not self.running:
                    break
                
                try:
                    msg = line.decode('utf-8', errors='replace').strip()
                    
                    # Detect frame progress (indicates data is flowing)
                    if 'frame=' in msg.lower():
                        # Update data time when frames are being processed
                        self.update_data_time()
                    
                    # Detect RTMP-specific errors
                    if 'rtmp' in msg.lower():
                        if any(x in msg.lower() for x in ['error', 'failed', 'could not', 'connection']):
                            print(f"[{self.name}] ⚠️  RTMP ERROR: {msg}", file=sys.stderr, flush=True)
                        elif 'connected' in msg.lower() or 'publishing' in msg.lower():
                            print(f"[{self.name}] ✓ RTMP: {msg}", flush=True)
                        else:
                            print(f"[{self.name}] RTMP: {msg}", flush=True)
                    # Filter informative messages
                    elif any(x in msg.lower() for x in ['error', 'warning', 'failed']):
                        print(f"[{self.name}] {msg}", file=sys.stderr, flush=True)
                    elif 'frame=' in msg.lower() or 'time=' in msg.lower():
                        # Progress messages - only log occasionally
                        pass
                    elif msg:
                        # Other messages
                        print(f"[{self.name}] {msg}", flush=True)
                        
                except Exception as e:
                    print(f"[{self.name}] Error processing stderr: {e}", flush=True)
                    
        except Exception as e:
            if self.running:
                print(f"[{self.name}] Stderr monitor error: {e}", 
                      file=sys.stderr, flush=True)
        
        print(f"[{self.name}] Stderr monitor stopped", flush=True)


class FFmpegProcessManager:
    """Manages FFmpeg processes for offline video and SRT input."""
    
    def __init__(self, config: StreamConfig):
        """Initialize FFmpeg process manager.
        
        Args:
            config: Stream configuration
        """
        self.config = config
        
        # Build FFmpeg command for offline video
        self.offline_args = self._build_offline_args()
        self.offline_process = FFmpegProcess(
            "ffmpeg-offline",
            self.offline_args
        )
        
        # Build FFmpeg command for SRT input
        self.srt_args = self._build_srt_args()
        self.srt_process = FFmpegProcess(
            "ffmpeg-srt",
            self.srt_args,
            data_callback=lambda: None  # Will be set by stream_switcher
        )
        
        print("=" * 80, flush=True)
        print("✅ V4.0 - ONLY 2 FFMPEG PROCESSES (offline + SRT, NO output process) ✅", flush=True)
        print("=" * 80, flush=True)
        print("[ffmpeg-mgr] FFmpeg process manager initialized", flush=True)
    
    def _build_offline_args(self) -> list:
        """Build FFmpeg arguments for offline video looping.
        
        Returns:
            List of FFmpeg command arguments
        """
        return [
            'ffmpeg',
            '-stream_loop', '-1',  # Loop infinitely
            '-re',  # Real-time playback
            '-i', self.config.fallback_video,
            
            # Video encoding
            '-c:v', 'libx264',
            '-preset', self.config.ffmpeg_preset,
            '-tune', self.config.ffmpeg_tune,
            '-b:v', f'{self.config.output_bitrate}k',
            '-maxrate', f'{self.config.output_bitrate}k',
            '-bufsize', f'{self.config.output_bitrate}k',  # 1x instead of 2x
            '-g', str(int(self.config.output_fps * self.config.gop_duration)),
            '-keyint_min', str(int(self.config.output_fps * self.config.gop_duration)),
            '-pix_fmt', 'yuv420p',
            '-vf', f'scale={self.config.output_width}:{self.config.output_height}:'
                   'force_original_aspect_ratio=decrease,'
                   f'pad={self.config.output_width}:{self.config.output_height}:-1:-1',
            '-r', str(self.config.output_fps),
            
            # Audio encoding
            '-c:a', 'aac',
            '-b:a', f'{self.config.ffmpeg_audio_bitrate}k',
            '-ar', '48000',
            '-ac', '2',
            
            # Output
            '-f', 'flv',
            'pipe:1'  # Output to stdout
        ]
    
    def _build_srt_args(self) -> list:
        """Build FFmpeg arguments for SRT input.
        
        Returns:
            List of FFmpeg command arguments
        """
        srt_uri = f'srt://0.0.0.0:{self.config.srt_port}?mode=listener&timeout=5000000'
        
        args = ['ffmpeg']
        
        # Add low-latency flags if enabled (before -i)
        if self.config.low_latency_mode:
            args.extend([
                '-fflags', 'nobuffer+flush_packets',
                '-flags', 'low_delay',
                '-probesize', '500000',           # 500KB (vs 5MB default) - sufficient for audio
                '-analyzeduration', '1000000',    # 1s (vs 5s default) - allows proper audio detection
            ])
        
        args.extend([
            '-i', srt_uri,
            
            # Video encoding  
            '-c:v', 'libx264',
            '-preset', self.config.ffmpeg_preset,
            '-tune', self.config.ffmpeg_tune,
            '-b:v', f'{self.config.output_bitrate}k',
            '-maxrate', f'{self.config.output_bitrate}k',
            '-bufsize', f'{self.config.output_bitrate}k',  # 1x instead of 2x
            '-g', str(int(self.config.output_fps * self.config.gop_duration)),
            '-keyint_min', str(int(self.config.output_fps * self.config.gop_duration)),
            '-pix_fmt', 'yuv420p',
            '-vf', f'scale={self.config.output_width}:{self.config.output_height}:'
                   'force_original_aspect_ratio=decrease,'
                   f'pad={self.config.output_width}:{self.config.output_height}:-1:-1',
            '-r', str(self.config.output_fps),
            
            # Audio encoding
            '-c:a', 'aac',
            '-b:a', f'{self.config.ffmpeg_audio_bitrate}k',
            '-ar', '48000',
            '-ac', '2',
            
            # Output
            '-f', 'flv',
            'pipe:1'  # Output to stdout
        ])
        
        # DEBUG: Print full command
        print(f"[ffmpeg-mgr] SRT FFmpeg command: {' '.join(args)}", flush=True)
        
        return args
    
    def start_offline(self) -> bool:
        """Start offline video FFmpeg process.
        
        Returns:
            True if started successfully
        """
        return self.offline_process.start()
    
    def start_srt(self) -> bool:
        """Start SRT input FFmpeg process.
        
        Returns:
            True if started successfully
        """
        return self.srt_process.start()
    
    def stop_offline(self) -> None:
        """Stop offline video FFmpeg process."""
        self.offline_process.stop()
    
    def stop_srt(self) -> None:
        """Stop SRT input FFmpeg process."""
        self.srt_process.stop()
    
    def stop_all(self) -> None:
        """Stop all FFmpeg processes."""
        print("[ffmpeg-mgr] Stopping all FFmpeg processes...", flush=True)
        self.stop_offline()
        self.stop_srt()
        print("[ffmpeg-mgr] ✓ All FFmpeg processes stopped", flush=True)
    
    def get_offline_fd(self) -> Optional[int]:
        """Get file descriptor for offline video stdout.
        
        Returns:
            File descriptor or None
        """
        return self.offline_process.get_stdout()
    
    def get_srt_fd(self) -> Optional[int]:
        """Get file descriptor for SRT stdout.
        
        Returns:
            File descriptor or None
        """
        return self.srt_process.get_stdout()
    
    def set_srt_data_callback(self, callback: Callable) -> None:
        """Set callback for SRT data flow detection.
        
        Args:
            callback: Function to call when SRT data flows
        """
        self.srt_process.data_callback = callback
    
    def get_status(self) -> dict:
        """Get status of all FFmpeg processes.
        
        Returns:
            Dictionary with process status
        """
        return {
            'offline': {
                'running': self.offline_process.is_running(),
                'pid': self.offline_process.process.pid if self.offline_process.process else None,
                'last_data_age': self.offline_process.get_last_data_age()
            },
            'srt': {
                'running': self.srt_process.is_running(),
                'pid': self.srt_process.process.pid if self.srt_process.process else None,
                'last_data_age': self.srt_process.get_last_data_age()
            }
        }