"""HTTP API server for controlling the stream switcher."""

import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs
from typing import Callable, Dict, Any

from stream_switcher import StreamSwitcher


class APIRequestHandler(BaseHTTPRequestHandler):
    """HTTP request handler for stream switcher API.
    
    Handles all REST endpoints for monitoring and controlling the switcher.
    """
    
    # Class variable to hold the switcher instance
    switcher: StreamSwitcher = None
    
    def log_message(self, format: str, *args) -> None:
        """Suppress default request logging."""
        pass
    
    def do_GET(self) -> None:
        """Handle GET requests."""
        parsed = urlparse(self.path)
        
        handlers = {
            "/health": self._handle_health,
            "/scene": self._handle_get_scene,
            "/scene/mode": self._handle_get_scene_mode,
            "/switch": self._handle_switch,
        }
        
        handler = handlers.get(parsed.path)
        if handler:
            handler(parsed)
        else:
            self._send_not_found()
    
    def do_POST(self) -> None:
        """Handle POST requests."""
        parsed = urlparse(self.path)
        
        handlers = {
            "/scene/camera": self._handle_scene_camera,
            "/scene/privacy": self._handle_scene_privacy,
        }
        
        handler = handlers.get(parsed.path)
        if handler:
            handler(parsed)
        else:
            self._send_not_found()
    
    def _handle_health(self, parsed: urlparse) -> None:
        """Handle /health endpoint."""
        try:
            status = self.switcher.get_status()
            self._send_json_response(status, 200)
        except Exception as e:
            self._send_error_response(500, str(e))
    
    def _handle_get_scene(self, parsed: urlparse) -> None:
        """Handle /scene endpoint."""
        try:
            scene = self.switcher.current_source
            self._send_json_response({"scene": scene}, 200)
        except Exception as e:
            self._send_error_response(500, str(e))
    
    def _handle_get_scene_mode(self, parsed: urlparse) -> None:
        """Handle /scene/mode endpoint."""
        try:
            mode = {"mode": self.switcher.scene_mode}
            self._send_json_response(mode, 200)
        except Exception as e:
            self._send_error_response(500, str(e))
    
    def _handle_switch(self, parsed: urlparse) -> None:
        """Handle /switch endpoint."""
        src = parse_qs(parsed.query).get("src", [""])[0].lower()
        print(f"[http] Switch request: {src}", flush=True)
        
        try:
            self.switcher.manual_switch(src)
            self._send_text_response(f"Switched to {src}\n", 200)
        except Exception as e:
            print(f"[http] Switch error: {e}", flush=True)
            self._send_error_response(400, str(e))
    
    def _handle_scene_camera(self, parsed: urlparse) -> None:
        """Handle /scene/camera endpoint."""
        print("[http] Scene camera mode request", flush=True)
        try:
            success = self.switcher.set_scene_mode("camera")
            if success:
                response = {"success": True, "message": "Camera mode enabled", "mode": "camera"}
                self._send_json_response(response, 200)
            else:
                response = {"success": False, "message": "Failed to enable camera mode"}
                self._send_json_response(response, 500)
        except Exception as e:
            print(f"[http] Scene camera error: {e}", flush=True)
            self._send_json_response({"success": False, "error": str(e)}, 500)
    
    def _handle_scene_privacy(self, parsed: urlparse) -> None:
        """Handle /scene/privacy endpoint."""
        print("[http] Scene privacy mode request", flush=True)
        try:
            success = self.switcher.set_scene_mode("privacy")
            if success:
                response = {"success": True, "message": "Privacy mode enabled", "mode": "privacy"}
                self._send_json_response(response, 200)
            else:
                response = {"success": False, "message": "Failed to enable privacy mode"}
                self._send_json_response(response, 500)
        except Exception as e:
            print(f"[http] Scene privacy error: {e}", flush=True)
            self._send_json_response({"success": False, "error": str(e)}, 500)
    
    def _send_json_response(self, data: Dict[str, Any], status: int) -> None:
        """Send JSON response."""
        try:
            response = json.dumps(data, indent=2)
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(response.encode())
        except BrokenPipeError:
            pass
    
    def _send_text_response(self, text: str, status: int) -> None:
        """Send plain text response."""
        try:
            self.send_response(status)
            self.end_headers()
            self.wfile.write(text.encode())
        except BrokenPipeError:
            pass
    
    def _send_error_response(self, status: int, error: str) -> None:
        """Send error response."""
        try:
            self.send_response(status)
            self.end_headers()
            self.wfile.write(f"Error: {error}\n".encode())
        except BrokenPipeError:
            pass
    
    def _send_not_found(self) -> None:
        """Send 404 response."""
        try:
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b"Not found\n")
        except BrokenPipeError:
            pass


class APIServer:
    """HTTP API server for the stream switcher.
    
    Provides REST endpoints for monitoring and controlling the stream.
    """
    
    def __init__(self, switcher: StreamSwitcher, port: int):
        """Initialize API server.
        
        Args:
            switcher: StreamSwitcher instance to control
            port: Port to listen on
        """
        self.switcher = switcher
        self.port = port
        self.server = None
        self.thread = None
        
        # Set the switcher in the handler class
        APIRequestHandler.switcher = switcher
    
    def start(self) -> None:
        """Start the API server in a background thread."""
        self.server = HTTPServer(("0.0.0.0", self.port), APIRequestHandler)
        
        print(f"[http] API server listening on 0.0.0.0:{self.port}", flush=True)
        print(f"[http] Endpoints:", flush=True)
        print(f"[http]   GET  /health", flush=True)
        print(f"[http]   GET  /scene", flush=True)
        print(f"[http]   GET  /scene/mode", flush=True)
        print(f"[http]   GET  /switch?src=srt|fallback|offline", flush=True)
        print(f"[http]   POST /scene/camera", flush=True)
        print(f"[http]   POST /scene/privacy", flush=True)
        
        self.thread = threading.Thread(target=self._run_server, daemon=True)
        self.thread.start()
    
    def _run_server(self) -> None:
        """Run the server (called in background thread)."""
        self.server.serve_forever()
    
    def stop(self) -> None:
        """Stop the API server."""
        if self.server:
            print("[http] Stopping API server...", flush=True)
            self.server.shutdown()
            print("[http] ✓ API server stopped", flush=True)