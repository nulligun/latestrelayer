#!/usr/bin/env python3
"""
State Manager for Compositor
Handles state transitions and tracking for the compositor pipeline.
"""
import time
from config import (
    STATE_FALLBACK_ONLY,
    STATE_VIDEO_BUFFERING,
    STATE_VIDEO_CONNECTED,
    STATE_SRT_BUFFERING,
    STATE_SRT_CONNECTED,
)


class StateManager:
    """Manages compositor state transitions and buffer tracking."""
    
    def __init__(self):
        # Current state
        self.state = STATE_FALLBACK_ONLY
        
        # Connection tracking
        self.video_ever_connected = False
        self.srt_ever_connected = False
        self.srt_connected = False
        self.tcp_connected = False
        
        # Buffer timestamps
        self.last_video_buf_time = time.time()
        self.last_srt_buf_time = time.time()
        
        # Buffer tracking for smooth switching
        self.video_first_buffer_time = None
        self.srt_first_buffer_time = None
        self.video_buffer_scheduled = False
        self.srt_buffer_scheduled = False
        
        # Restart flags
        self.video_restart_scheduled = False
        self.restart_scheduled = False
        
        # Grace period tracking
        self.srt_disconnect_grace_scheduled = False
        
        # Statistics
        self.tcp_bytes_received = 0
        self.srt_bitrate_kbps = 0
    
    def get_current_scene(self):
        """Get the current scene name based on active state."""
        if self.state == STATE_SRT_CONNECTED:
            return "SRT"
        elif self.state == STATE_VIDEO_CONNECTED:
            return "VIDEO"
        else:
            return "BLACK"
    
    def set_state(self, new_state):
        """Set the state with logging."""
        if self.state != new_state:
            print(f"[state] Transition: {self.state} → {new_state}", flush=True)
            self.state = new_state
    
    def update_video_buffer_time(self):
        """Update the last video buffer timestamp."""
        self.last_video_buf_time = time.time()
    
    def update_srt_buffer_time(self):
        """Update the last SRT buffer timestamp."""
        self.last_srt_buf_time = time.time()
    
    def start_video_buffering(self):
        """Mark the start of video buffering."""
        if self.video_first_buffer_time is None:
            self.video_first_buffer_time = time.time()
            self.set_state(STATE_VIDEO_BUFFERING)
            return True
        return False
    
    def start_srt_buffering(self):
        """Mark the start of SRT buffering."""
        if self.srt_first_buffer_time is None:
            self.srt_first_buffer_time = time.time()
            self.set_state(STATE_SRT_BUFFERING)
            return True
        return False
    
    def cancel_video_buffering(self):
        """Cancel video buffering and reset."""
        self.video_first_buffer_time = None
        self.video_buffer_scheduled = False
        if self.state == STATE_VIDEO_BUFFERING:
            self.set_state(STATE_FALLBACK_ONLY)
    
    def cancel_srt_buffering(self):
        """Cancel SRT buffering and reset."""
        self.srt_first_buffer_time = None
        self.srt_buffer_scheduled = False
        if self.state == STATE_SRT_BUFFERING:
            # Fall back to video if available, otherwise fallback
            if self.video_ever_connected:
                self.set_state(STATE_VIDEO_CONNECTED)
            else:
                self.set_state(STATE_FALLBACK_ONLY)
    
    def reset_video_buffers(self):
        """Reset video buffer tracking."""
        self.video_first_buffer_time = None
        self.video_buffer_scheduled = False
    
    def reset_srt_buffers(self):
        """Reset SRT buffer tracking."""
        self.srt_first_buffer_time = None
        self.srt_buffer_scheduled = False
    
    def is_video_active(self):
        """Check if video is currently active."""
        return self.state in (STATE_VIDEO_CONNECTED, STATE_VIDEO_BUFFERING)
    
    def is_srt_active(self):
        """Check if SRT is currently active."""
        return self.state in (STATE_SRT_CONNECTED, STATE_SRT_BUFFERING)
    
    def can_switch_to_video(self):
        """Check if we can switch to video."""
        return self.state in (STATE_FALLBACK_ONLY, STATE_SRT_CONNECTED)
    
    def can_switch_to_srt(self):
        """Check if we can switch to SRT."""
        return self.state in (STATE_FALLBACK_ONLY, STATE_VIDEO_CONNECTED)