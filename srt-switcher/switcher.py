"""!
SRT Stream Switcher - Dual-Pipeline Architecture
Seamlessly switches between offline video and SRT input at the encoded (FLV) level
"""

import sys
import os
import signal
import traceback

# Force unbuffered output
sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

print("=" * 60, flush=True)
print("SRT Stream Switcher v2.0 - Dual-Pipeline Architecture", flush=True)
print("=" * 60, flush=True)

# Initialize GStreamer
import gi
gi.require_version('Gst', '1.0')
gi.require_version('GObject', '2.0')
from gi.repository import Gst, GLib

print("[startup] Initializing GStreamer...", flush=True)
Gst.init(None)
print("[startup] ✓ GStreamer initialized", flush=True)

# Import our refactored modules
from config import StreamConfig
from stream_switcher import StreamSwitcher
from api_server import APIServer


def main() -> int:
    """Main entry point for the SRT Stream Switcher.
    
    Returns:
        Exit code (0 for success, 1 for error)
    """
    print("[main] Initializing SRT Switcher v2.0...", flush=True)
    
    switcher = None
    api_server = None
    
    try:
        # Load and validate configuration
        config = StreamConfig.from_environment()
        config.validate()
        config.print_config()
        
        # Create stream switcher
        switcher = StreamSwitcher(config)
        
        # Create and start API server
        api_server = APIServer(switcher, config.api_port)
        api_server.start()
        
        # Start the stream switcher pipeline
        if not switcher.start():
            raise RuntimeError("Failed to start pipeline")
        
        # Set up signal handlers for graceful shutdown
        loop = GLib.MainLoop()
        
        def signal_handler(signum: int, frame) -> None:
            """Handle shutdown signals."""
            signame = "SIGTERM" if signum == signal.SIGTERM else "SIGINT"
            print(f"\n[main] Received {signame}, shutting down...", flush=True)
            loop.quit()
        
        signal.signal(signal.SIGTERM, signal_handler)
        signal.signal(signal.SIGINT, signal_handler)
        
        print("[main] ✓ SRT Switcher v2.0 is running", flush=True)
        print("[main] Offline video playing, waiting for SRT connections...", flush=True)
        
        # Run main loop
        loop.run()
        
    except Exception as e:
        print(f"[ERROR] Startup failed: {e}", file=sys.stderr, flush=True)
        traceback.print_exc()
        return 1
    
    finally:
        # Clean shutdown
        if api_server:
            api_server.stop()
        if switcher:
            switcher.stop()
    
    print("[main] Shutdown complete", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())