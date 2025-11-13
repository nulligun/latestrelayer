"""SRT connection monitoring for detecting when stream is active."""

import time
import threading
import traceback
from typing import Callable

import gi
gi.require_version('GLib', '2.0')
from gi.repository import GLib


class SRTMonitor(threading.Thread):
    """Monitor thread that tracks SRT connection status.
    
    Uses packet timestamps to detect when SRT data is flowing.
    Automatically triggers callbacks when connection status changes.
    """
    
    def __init__(self, timeout: float, 
                 on_connected: Callable[[], None],
                 on_disconnected: Callable[[], None],
                 interval: float = 1.0):
        """Initialize SRT monitor.
        
        Args:
            timeout: Seconds without data before considering connection lost
            on_connected: Callback when SRT connection detected
            on_disconnected: Callback when SRT connection lost
            interval: Seconds between connection checks (default: 1.0)
        """
        super().__init__(daemon=True)
        self.timeout = timeout
        self.on_connected = on_connected
        self.on_disconnected = on_disconnected
        self.interval = interval
        
        self.last_packet_time: float = None
        self.is_connected = False
        self.running = True
        self.lock = threading.Lock()
        
        print("[monitor] SRT monitor thread initialized", flush=True)
        print(f"[monitor] Check interval: {self.interval}s, timeout: {self.timeout}s", flush=True)
    
    def update_packet_time(self) -> None:
        """Called when SRT data is received.
        
        This should be called from a GStreamer probe callback.
        """
        with self.lock:
            self.last_packet_time = time.time()
    
    def run(self) -> None:
        """Main monitoring loop.
        
        Checks connection status every second and triggers callbacks
        when state changes.
        """
        print("[monitor] Starting SRT connection monitor", flush=True)
        
        while self.running:
            try:
                self._check_connection_status()
                time.sleep(self.interval)
            except Exception as e:
                print(f"[monitor] Error in monitor loop: {e}", flush=True)
                traceback.print_exc()
    
    def _check_connection_status(self) -> None:
        """Check if connection is active based on packet timestamps."""
        current_time = time.time()
        
        with self.lock:
            if not self.last_packet_time:
                return
            
            time_since_packet = current_time - self.last_packet_time
            
            if time_since_packet < self.timeout:
                # Data flowing - connection is alive
                if not self.is_connected:
                    self._trigger_connected(time_since_packet)
            else:
                # No recent data - connection lost
                if self.is_connected:
                    self._trigger_disconnected(time_since_packet)
    
    def _trigger_connected(self, data_age: float) -> None:
        """Trigger connection detected callback."""
        print(f"[monitor] ✓ SRT connection detected (data age: {data_age:.2f}s)", flush=True)
        self.is_connected = True
        GLib.idle_add(self.on_connected)
    
    def _trigger_disconnected(self, time_since_data: float) -> None:
        """Trigger connection lost callback."""
        print(f"[monitor] ✗ SRT connection lost (no data for {time_since_data:.2f}s)", flush=True)
        self.is_connected = False
        GLib.idle_add(self.on_disconnected)
    
    def stop(self) -> None:
        """Stop the monitor thread gracefully."""
        print("[monitor] Stopping SRT monitor", flush=True)
        self.running = False
    
    def get_connection_status(self) -> bool:
        """Get current connection status thread-safely.
        
        Returns:
            True if SRT is connected, False otherwise
        """
        with self.lock:
            return self.is_connected