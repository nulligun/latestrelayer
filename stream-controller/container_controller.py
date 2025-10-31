#!/usr/bin/env python3
import sys
import json
import os
from urllib.parse import urlparse
from http.server import BaseHTTPRequestHandler, HTTPServer

# Force unbuffered output
sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

print("=" * 60, flush=True)
print("Container Controller - Starting Up", flush=True)
print("=" * 60, flush=True)

import docker

print("[startup] Initializing Docker client...", flush=True)
try:
    client = docker.from_env()
    print("[startup] ✓ Docker client initialized", flush=True)
except Exception as e:
    print(f"[startup] ERROR: Failed to initialize Docker client: {e}", file=sys.stderr)
    sys.exit(1)

# Get project name from environment or use default
PROJECT_NAME = os.getenv("COMPOSE_PROJECT_NAME", "relayer")
print(f"[startup] Project name: {PROJECT_NAME}", flush=True)


class ContainerController:
    """Manages Docker container lifecycle operations."""
    
    def __init__(self, project_name):
        self.client = client
        self.project_name = project_name
        print(f"[controller] Initialized with project name: {project_name}")
    
    def get_container_name(self, short_name):
        """Convert short name to full container name with project prefix."""
        return f"{self.project_name}-{short_name}"
    
    def start_container(self, short_name):
        """Start a container by short name."""
        container_name = self.get_container_name(short_name)
        print(f"[controller] Starting container: {container_name}")
        
        try:
            container = self.client.containers.get(container_name)
            
            # Check if already running
            container.reload()
            if container.status == "running":
                print(f"[controller] Container {container_name} is already running")
                return {
                    "status": "already_running",
                    "container": short_name,
                    "message": f"Container {short_name} is already running"
                }
            
            # Start the container
            container.start()
            print(f"[controller] ✓ Container {container_name} started successfully")
            
            return {
                "status": "started",
                "container": short_name,
                "message": f"Container {short_name} started successfully"
            }
            
        except docker.errors.NotFound:
            error_msg = f"Container not found: {short_name}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise ValueError(error_msg)
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise RuntimeError(error_msg)
    
    def stop_container(self, short_name):
        """Stop a container by short name."""
        container_name = self.get_container_name(short_name)
        print(f"[controller] Stopping container: {container_name}")
        
        try:
            container = self.client.containers.get(container_name)
            
            # Check if already stopped
            container.reload()
            if container.status != "running":
                print(f"[controller] Container {container_name} is already stopped")
                return {
                    "status": "already_stopped",
                    "container": short_name,
                    "message": f"Container {short_name} is already stopped"
                }
            
            # Stop the container
            container.stop(timeout=10)
            print(f"[controller] ✓ Container {container_name} stopped successfully")
            
            return {
                "status": "stopped",
                "container": short_name,
                "message": f"Container {short_name} stopped successfully"
            }
            
        except docker.errors.NotFound:
            error_msg = f"Container not found: {short_name}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise ValueError(error_msg)
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise RuntimeError(error_msg)
    
    def restart_container(self, short_name):
        """Restart a container by short name."""
        container_name = self.get_container_name(short_name)
        print(f"[controller] Restarting container: {container_name}")
        
        try:
            container = self.client.containers.get(container_name)
            container.restart(timeout=10)
            print(f"[controller] ✓ Container {container_name} restarted successfully")
            
            return {
                "status": "restarted",
                "container": short_name,
                "message": f"Container {short_name} restarted successfully"
            }
            
        except docker.errors.NotFound:
            error_msg = f"Container not found: {short_name}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise ValueError(error_msg)
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise RuntimeError(error_msg)
    
    def get_status(self, short_name):
        """Get the status of a container by short name."""
        container_name = self.get_container_name(short_name)
        
        try:
            container = self.client.containers.get(container_name)
            container.reload()
            
            status_info = {
                "container": short_name,
                "status": container.status,
                "running": container.status == "running",
                "id": container.short_id
            }
            
            print(f"[controller] Status for {container_name}: {container.status}")
            return status_info
            
        except docker.errors.NotFound:
            error_msg = f"Container not found: {short_name}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise ValueError(error_msg)
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise RuntimeError(error_msg)
    
    def list_containers(self):
        """List all containers with the project prefix."""
        try:
            all_containers = self.client.containers.list(all=True)
            project_containers = []
            
            for container in all_containers:
                if container.name.startswith(f"{self.project_name}-"):
                    short_name = container.name[len(f"{self.project_name}-"):]
                    project_containers.append({
                        "name": short_name,
                        "full_name": container.name,
                        "status": container.status,
                        "running": container.status == "running",
                        "id": container.short_id
                    })
            
            print(f"[controller] Listed {len(project_containers)} containers for project {self.project_name}")
            return {"containers": project_containers}
            
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise RuntimeError(error_msg)


# Initialize the controller
controller = ContainerController(PROJECT_NAME)


class Handler(BaseHTTPRequestHandler):
    """HTTP request handler for container control API."""
    
    def log_message(self, format, *args):
        """Suppress default request logging."""
        pass
    
    def send_json(self, data, status=200):
        """Helper to send JSON responses."""
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(data, indent=2).encode() + b"\n")
    
    def send_text(self, text, status=200):
        """Helper to send plain text responses."""
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(f"{text}\n".encode())
    
    def do_POST(self):
        """Handle POST requests for container operations."""
        parsed = urlparse(self.path)
        path_parts = parsed.path.strip("/").split("/")
        
        # Expected format: /container/<name>/<action>
        if len(path_parts) == 3 and path_parts[0] == "container":
            short_name = path_parts[1]
            action = path_parts[2]
            
            print(f"[http] POST request: {action} container {short_name}")
            
            try:
                if action == "start":
                    result = controller.start_container(short_name)
                    self.send_json(result)
                elif action == "stop":
                    result = controller.stop_container(short_name)
                    self.send_json(result)
                elif action == "restart":
                    result = controller.restart_container(short_name)
                    self.send_json(result)
                else:
                    error = {"error": f"Unknown action: {action}"}
                    print(f"[http] ERROR: {error['error']}", file=sys.stderr)
                    self.send_json(error, 404)
                    
            except ValueError as e:
                error = {"error": str(e)}
                print(f"[http] ERROR: {error['error']}", file=sys.stderr)
                self.send_json(error, 404)
            except RuntimeError as e:
                error = {"error": str(e)}
                print(f"[http] ERROR: {error['error']}", file=sys.stderr)
                self.send_json(error, 500)
            except Exception as e:
                error = {"error": f"Unexpected error: {str(e)}"}
                print(f"[http] ERROR: {error['error']}", file=sys.stderr)
                self.send_json(error, 500)
        else:
            error = {"error": "Invalid endpoint. Use /container/<name>/<action>"}
            self.send_json(error, 404)
    
    def do_GET(self):
        """Handle GET requests for status and health checks."""
        parsed = urlparse(self.path)
        path_parts = parsed.path.strip("/").split("/")
        
        # Health check endpoint
        if parsed.path == "/health":
            print("[http] Health check request")
            self.send_text("ok")
            return
        
        # List all containers endpoint
        if parsed.path == "/containers":
            print("[http] List containers request")
            try:
                result = controller.list_containers()
                self.send_json(result)
            except Exception as e:
                error = {"error": str(e)}
                print(f"[http] ERROR: {error['error']}", file=sys.stderr)
                self.send_json(error, 500)
            return
        
        # Container status endpoint: /container/<name>/status
        if len(path_parts) == 3 and path_parts[0] == "container" and path_parts[2] == "status":
            short_name = path_parts[1]
            print(f"[http] Status request for container: {short_name}")
            
            try:
                result = controller.get_status(short_name)
                self.send_json(result)
            except ValueError as e:
                error = {"error": str(e)}
                print(f"[http] ERROR: {error['error']}", file=sys.stderr)
                self.send_json(error, 404)
            except Exception as e:
                error = {"error": str(e)}
                print(f"[http] ERROR: {error['error']}", file=sys.stderr)
                self.send_json(error, 500)
            return
        
        # Unknown endpoint
        error = {"error": "Not found"}
        self.send_json(error, 404)


def run_http():
    """Start the HTTP server."""
    srv = HTTPServer(("0.0.0.0", 8089), Handler)
    print("[http] HTTP server bound to 0.0.0.0:8089")
    print("[http] Endpoints:")
    print("[http]   POST /container/<name>/start   - Start a container")
    print("[http]   POST /container/<name>/stop    - Stop a container")
    print("[http]   POST /container/<name>/restart - Restart a container")
    print("[http]   GET  /container/<name>/status  - Get container status")
    print("[http]   GET  /containers               - List all containers")
    print("[http]   GET  /health                   - Health check")
    print("[http] Ready to accept requests")
    srv.serve_forever()


if __name__ == "__main__":
    print("[main] Starting container controller...")
    
    try:
        run_http()
    except KeyboardInterrupt:
        print("[main] Received interrupt signal")
    except Exception as e:
        print(f"[main] ERROR: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
    finally:
        print("[main] Shutdown complete")