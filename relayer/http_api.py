#!/usr/bin/env python3
"""
HTTP API for Compositor
Provides HTTP endpoints for compositor status and control.
"""
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from config import HTTP_API_PORT


class CompositorHTTPHandler(BaseHTTPRequestHandler):
    """HTTP request handler for compositor API."""
    
    # Class variable to hold reference to compositor manager
    compositor_manager = None
    
    def log_message(self, format, *args):
        """Override to silence HTTP request logging."""
        pass
    
    def send_json_response(self, data, status=200):
        """Send JSON response."""
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())
    
    def do_OPTIONS(self):
        """Handle CORS preflight."""
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()
    
    def do_GET(self):
        """Handle GET requests."""
        start_time = time.perf_counter()
        path = self.path
        
        if path == '/health':
            # Get SRT connection status, bitrate, and current scene
            srt_stats = self.compositor_manager.get_srt_stats()
            scene = self.compositor_manager.get_current_scene()
            response = {
                'status': 'ok',
                'scene': scene,
                'srt_connected': srt_stats['connected'],
                'srt_bitrate_kbps': srt_stats['bitrate_kbps'],
                'privacy_enabled': self.compositor_manager.get_privacy_enabled()
            }
            self.send_json_response(response)
        
        elif path == '/privacy':
            enabled = self.compositor_manager.get_privacy_enabled()
            self.send_json_response({'enabled': enabled})
        
        else:
            self.send_json_response({'error': 'Not found'}, 404)
        
        # Log request timing
        elapsed = time.perf_counter() - start_time
        if elapsed > 0.01:  # Log if > 10ms
            print(f"[http-profile] GET {path} took {elapsed*1000:.2f}ms", flush=True)
    
    def do_POST(self):
        """Handle POST requests."""
        start_time = time.perf_counter()
        path = self.path
        
        if path == '/privacy':
            content_length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(content_length)
            
            try:
                data = json.loads(body.decode()) if body else {}
                enabled = data.get('enabled', False)
                
                self.compositor_manager.set_privacy_mode(enabled)
                self.send_json_response({
                    'success': True,
                    'enabled': enabled,
                    'message': f"Privacy mode {'enabled' if enabled else 'disabled'}"
                })
            except Exception as e:
                print(f"[http] Error handling privacy request: {e}", flush=True)
                self.send_json_response({'error': str(e)}, 400)
        
        else:
            self.send_json_response({'error': 'Not found'}, 404)
        
        # Log request timing
        elapsed = time.perf_counter() - start_time
        if elapsed > 0.01:  # Log if > 10ms
            print(f"[http-profile] POST {path} took {elapsed*1000:.2f}ms", flush=True)


def run_http_server(compositor_manager):
    """
    Run HTTP API server in background thread.
    
    Args:
        compositor_manager: CompositorManager instance to handle requests
    """
    CompositorHTTPHandler.compositor_manager = compositor_manager
    server = HTTPServer(('0.0.0.0', HTTP_API_PORT), CompositorHTTPHandler)
    print(f"[http] HTTP API server listening on port {HTTP_API_PORT}", flush=True)
    print("[http] Endpoints:", flush=True)
    print("[http]   GET  /health  - Health check with scene info", flush=True)
    print("[http]   GET  /privacy - Get privacy mode status", flush=True)
    print("[http]   POST /privacy - Set privacy mode (JSON: {\"enabled\": true/false})", flush=True)
    server.serve_forever()