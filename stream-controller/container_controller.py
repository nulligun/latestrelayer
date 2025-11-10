#!/usr/bin/env python3
import sys
import json
import os
import yaml
import subprocess
import threading
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

# Compose file path
COMPOSE_FILE = "/app/docker-compose.yml"


class ComposeParser:
    """Parse docker-compose.yml to get service definitions."""
    
    def __init__(self, compose_file):
        self.compose_file = compose_file
        self.services = {}
        self._parse()
    
    def _resolve_env_var(self, value):
        """Resolve environment variables in the format ${VAR:-default} or ${VAR}."""
        import re
        
        if not isinstance(value, str):
            return value
        
        # Pattern to match ${VAR:-default} or ${VAR}
        pattern = r'\$\{([^}:]+)(?::-)([^}]+)?\}'
        
        def replacer(match):
            var_name = match.group(1)
            default_value = match.group(2) if match.group(2) else ''
            # Get from environment, or use default
            return os.getenv(var_name, default_value)
        
        return re.sub(pattern, replacer, value)
    
    def _parse(self):
        """Parse the compose file and extract service information."""
        try:
            if not os.path.exists(self.compose_file):
                print(f"[parser] WARNING: Compose file not found: {self.compose_file}", file=sys.stderr)
                return
            
            with open(self.compose_file, 'r') as f:
                compose_data = yaml.safe_load(f)
            
            if not compose_data or 'services' not in compose_data:
                print("[parser] WARNING: No services found in compose file", file=sys.stderr)
                return
            
            for service_name, service_config in compose_data['services'].items():
                # Extract container name or generate it
                container_name = service_config.get('container_name', f"{PROJECT_NAME}-{service_name}")
                
                # Resolve environment variables in container name
                container_name = self._resolve_env_var(container_name)
                
                # Extract short name (remove project prefix)
                if container_name.startswith(f"{PROJECT_NAME}-"):
                    short_name = container_name[len(f"{PROJECT_NAME}-"):]
                else:
                    short_name = service_name
                
                # Check if service has manual profile
                profiles = service_config.get('profiles', [])
                is_manual = 'manual' in profiles
                
                self.services[short_name] = {
                    'name': short_name,
                    'full_name': container_name,
                    'service_name': service_name,
                    'is_manual': is_manual,
                    'profiles': profiles
                }
            
            print(f"[parser] Parsed {len(self.services)} services from compose file", flush=True)
            
        except Exception as e:
            print(f"[parser] ERROR: Failed to parse compose file: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
    
    def get_services(self):
        """Return dictionary of all services."""
        return self.services
    
    def get_service(self, short_name):
        """Get service info by short name."""
        return self.services.get(short_name)


class ContainerController:
    """Manages Docker container lifecycle operations."""
    
    def __init__(self, project_name, compose_parser):
        self.client = client
        self.project_name = project_name
        self.compose_parser = compose_parser
        print(f"[controller] Initialized with project name: {project_name}")
    
    def _get_health_status(self, container):
        """Extract health status from container attributes."""
        try:
            state = container.attrs['State']
            status = state.get('Status', 'unknown')
            
            # Health status only applies to running containers
            if status == 'running':
                health = state.get('Health', {})
                health_status = health.get('Status', '')
                # Return None if no healthcheck is configured
                return health_status if health_status else None
            
            return None
        except Exception as e:
            print(f"[controller] WARNING: Failed to get health status: {e}", file=sys.stderr)
            return None
    
    def _format_detailed_status(self, container):
        """Format detailed status string from container attributes."""
        try:
            state = container.attrs['State']
            status = state.get('Status', 'unknown')
            
            # For not-running containers
            if status == 'exited':
                exit_code = state.get('ExitCode', 0)
                finished_at = state.get('FinishedAt', '')
                if finished_at:
                    from datetime import datetime
                    try:
                        finished = datetime.fromisoformat(finished_at.replace('Z', '+00:00'))
                        now = datetime.now(finished.tzinfo)
                        delta = now - finished
                        time_ago = self._format_time_delta(delta)
                        return f"Exited ({exit_code}) {time_ago} ago"
                    except:
                        return f"Exited ({exit_code})"
                return f"Exited ({exit_code})"
            
            # For running containers
            elif status == 'running':
                started_at = state.get('StartedAt', '')
                if started_at:
                    from datetime import datetime
                    try:
                        started = datetime.fromisoformat(started_at.replace('Z', '+00:00'))
                        now = datetime.now(started.tzinfo)
                        delta = now - started
                        uptime = self._format_time_delta(delta)
                        
                        # Check for health status
                        health = state.get('Health', {})
                        health_status = health.get('Status', '')
                        
                        if health_status == 'healthy':
                            return f"Up {uptime} (healthy)"
                        elif health_status == 'unhealthy':
                            return f"Up {uptime} (unhealthy)"
                        elif health_status == 'starting':
                            return f"Up {uptime} (health: starting)"
                        else:
                            return f"Up {uptime}"
                    except:
                        return "Up"
                return "Up"
            
            # For other statuses
            elif status == 'created':
                return "Created"
            elif status == 'paused':
                return "Paused"
            elif status == 'restarting':
                return "Restarting"
            else:
                return status.capitalize()
                
        except Exception as e:
            print(f"[controller] WARNING: Failed to format detailed status: {e}", file=sys.stderr)
            return "Unknown"
    
    def _format_time_delta(self, delta):
        """Format a time delta into human-readable string."""
        total_seconds = int(delta.total_seconds())
        
        if total_seconds < 60:
            return f"{total_seconds} seconds"
        elif total_seconds < 3600:
            minutes = total_seconds // 60
            return f"{minutes} minute{'s' if minutes != 1 else ''}"
        elif total_seconds < 86400:
            hours = total_seconds // 3600
            return f"{hours} hour{'s' if hours != 1 else ''}"
        else:
            days = total_seconds // 86400
            return f"{days} day{'s' if days != 1 else ''}"
    
    def get_container_name(self, short_name):
        """Convert short name to full container name with project prefix."""
        return f"{self.project_name}-{short_name}"
    
    def _start_container_sync(self, short_name):
        """Synchronous start operation (run in background thread)."""
        container_name = self.get_container_name(short_name)
        
        try:
            container = self.client.containers.get(container_name)
            
            # Check if already running
            container.reload()
            if container.status == "running":
                print(f"[controller] Container {container_name} is already running")
                return
            
            # Start the container
            container.start()
            print(f"[controller] ✓ Container {container_name} started successfully")
            
        except docker.errors.NotFound:
            # Container doesn't exist, create and start it
            print(f"[controller] Container {container_name} not found, creating and starting...")
            self._create_and_start_container_sync(short_name)
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
        except Exception as e:
            print(f"[controller] ERROR in background start: {str(e)}", file=sys.stderr)
    
    def start_container(self, short_name):
        """Start a container by short name (async). Returns immediately."""
        container_name = self.get_container_name(short_name)
        print(f"[controller] Starting container: {container_name} (async)")
        
        # Start operation in background thread
        thread = threading.Thread(target=self._start_container_sync, args=(short_name,), daemon=True)
        thread.start()
        
        return {
            "status": "starting",
            "container": short_name,
            "message": f"Container {short_name} is starting"
        }
    
    def _stop_container_sync(self, short_name):
        """Synchronous stop operation (run in background thread)."""
        container_name = self.get_container_name(short_name)
        
        try:
            container = self.client.containers.get(container_name)
            
            # Check if already stopped
            container.reload()
            if container.status != "running":
                print(f"[controller] Container {container_name} is already stopped")
                return
            
            # Stop the container with 30 second timeout
            container.stop(timeout=30)
            print(f"[controller] ✓ Container {container_name} stopped successfully")
            
        except docker.errors.NotFound:
            error_msg = f"Container not found: {short_name}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
        except Exception as e:
            print(f"[controller] ERROR in background stop: {str(e)}", file=sys.stderr)
    
    def stop_container(self, short_name):
        """Stop a container by short name (async). Returns immediately."""
        container_name = self.get_container_name(short_name)
        print(f"[controller] Stopping container: {container_name} (async)")
        
        # Start operation in background thread
        thread = threading.Thread(target=self._stop_container_sync, args=(short_name,), daemon=True)
        thread.start()
        
        return {
            "status": "stopping",
            "container": short_name,
            "message": f"Container {short_name} is stopping"
        }
    
    def _restart_container_sync(self, short_name):
        """Synchronous restart operation (run in background thread)."""
        container_name = self.get_container_name(short_name)
        
        try:
            container = self.client.containers.get(container_name)
            container.restart(timeout=30)
            print(f"[controller] ✓ Container {container_name} restarted successfully")
            
        except docker.errors.NotFound:
            error_msg = f"Container not found: {short_name}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
        except Exception as e:
            print(f"[controller] ERROR in background restart: {str(e)}", file=sys.stderr)
    
    def restart_container(self, short_name):
        """Restart a container by short name (async). Returns immediately."""
        container_name = self.get_container_name(short_name)
        print(f"[controller] Restarting container: {container_name} (async)")
        
        # Start operation in background thread
        thread = threading.Thread(target=self._restart_container_sync, args=(short_name,), daemon=True)
        thread.start()
        
        return {
            "status": "restarting",
            "container": short_name,
            "message": f"Container {short_name} is restarting"
        }
    
    def get_status(self, short_name):
        """Get the status of a container by short name."""
        container_name = self.get_container_name(short_name)
        
        try:
            container = self.client.containers.get(container_name)
            container.reload()
            
            detailed_status = self._format_detailed_status(container)
            health_status = self._get_health_status(container)
            
            status_info = {
                "container": short_name,
                "status": container.status,
                "status_detail": detailed_status,
                "running": container.status == "running",
                "health": health_status,
                "id": container.short_id
            }
            
            print(f"[controller] Status for {container_name}: {container.status} ({detailed_status}) [health: {health_status}]")
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
        """List all containers, merging compose services with actual containers."""
        try:
            # Get all actual containers from Docker
            all_containers = self.client.containers.list(all=True)
            containers_by_name = {}
            
            for container in all_containers:
                if container.name.startswith(f"{self.project_name}-"):
                    short_name = container.name[len(f"{self.project_name}-"):]
                    detailed_status = self._format_detailed_status(container)
                    health_status = self._get_health_status(container)
                    containers_by_name[short_name] = {
                        "name": short_name,
                        "full_name": container.name,
                        "status": container.status,
                        "status_detail": detailed_status,
                        "running": container.status == "running",
                        "health": health_status,
                        "id": container.short_id,
                        "created": True
                    }
            
            # Get all services from compose file
            compose_services = self.compose_parser.get_services()
            
            # Merge: start with compose services, overlay actual container data
            merged_containers = []
            
            for short_name, service_info in compose_services.items():
                if short_name in containers_by_name:
                    # Container exists - use actual data
                    merged_containers.append(containers_by_name[short_name])
                else:
                    # Service defined but container doesn't exist
                    merged_containers.append({
                        "name": short_name,
                        "full_name": service_info['full_name'],
                        "status": "not-created",
                        "status_detail": "Not created",
                        "running": False,
                        "health": None,
                        "id": None,
                        "created": False,
                        "is_manual": service_info.get('is_manual', False)
                    })
            
            print(f"[controller] Listed {len(merged_containers)} services/containers for project {self.project_name}")
            return {"containers": merged_containers}
            
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise RuntimeError(error_msg)
    
    def _create_and_start_container_sync(self, short_name):
        """Synchronous create and start operation (run in background thread)."""
        try:
            # Get service info from compose parser
            service_info = self.compose_parser.get_service(short_name)
            if not service_info:
                error_msg = f"Service not found in compose file: {short_name}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return
            
            service_name = service_info['service_name']
            
            # Check if container already exists
            try:
                container_name = self.get_container_name(short_name)
                container = self.client.containers.get(container_name)
                
                # Container exists, check if running
                container.reload()
                if container.status == "running":
                    print(f"[controller] Container {container_name} is already running")
                    return
                else:
                    # Container exists but stopped, just start it
                    container.start()
                    print(f"[controller] ✓ Started existing container {container_name}")
                    return
            except docker.errors.NotFound:
                # Container doesn't exist, need to create it
                pass
            
            # Use docker compose to create and start the container
            cmd = ["docker", "compose", "-f", COMPOSE_FILE, "--env-file", "/app/.env", "up", "-d", service_name]
            
            print(f"[controller] Executing: {' '.join(cmd)}")
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=60
            )
            
            if result.returncode != 0:
                error_msg = f"Failed to create container: {result.stderr}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return
            
            print(f"[controller] ✓ Container {short_name} created and started successfully")
            print(f"[controller] Output: {result.stdout}")
            
        except subprocess.TimeoutExpired:
            error_msg = f"Timeout creating container: {short_name}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
        except Exception as e:
            print(f"[controller] ERROR in background create-and-start: {str(e)}", file=sys.stderr)
    
    def create_and_start_container(self, short_name):
        """Create and start a container using docker compose (async). Returns immediately."""
        print(f"[controller] Creating and starting container: {short_name} (async)")
        
        # Start operation in background thread
        thread = threading.Thread(target=self._create_and_start_container_sync, args=(short_name,), daemon=True)
        thread.start()
        
        return {
            "status": "creating",
            "container": short_name,
            "message": f"Container {short_name} is being created and started"
        }
    
    def get_logs(self, short_name, tail=500):
        """Get logs from a container."""
        container_name = self.get_container_name(short_name)
        
        try:
            container = self.client.containers.get(container_name)
            
            # Get logs with timestamps
            logs = container.logs(
                tail=tail,
                timestamps=True,
                stream=False
            )
            
            # Decode logs from bytes to string
            logs_str = logs.decode('utf-8', errors='replace')
            
            # Split into lines and filter empty lines
            log_lines = [line for line in logs_str.split('\n') if line.strip()]
            
            print(f"[controller] Retrieved {len(log_lines)} log lines for {container_name}")
            
            return {
                "container": short_name,
                "logs": log_lines,
                "count": len(log_lines)
            }
            
        except docker.errors.NotFound:
            error_msg = f"Container not found: {short_name}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise ValueError(error_msg)
        except docker.errors.APIError as e:
            error_msg = f"Docker API error: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise RuntimeError(error_msg)


# Initialize the compose parser and controller
print("[startup] Initializing compose parser...", flush=True)
compose_parser = ComposeParser(COMPOSE_FILE)

controller = ContainerController(PROJECT_NAME, compose_parser)


class Handler(BaseHTTPRequestHandler):
    """HTTP request handler for container control API."""
    
    def log_message(self, format, *args):
        """Suppress default request logging."""
        pass
    
    def send_json(self, data, status=200):
        """Helper to send JSON responses."""
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(data, indent=2).encode() + b"\n")
        except (BrokenPipeError, ConnectionResetError) as e:
            # Client closed connection before we could send response
            # This is common during concurrent requests or container lifecycle events
            print(f"[http] Client disconnected before response could be sent: {e.__class__.__name__}", file=sys.stderr)
    
    def send_text(self, text, status=200):
        """Helper to send plain text responses."""
        try:
            self.send_response(status)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(f"{text}\n".encode())
        except (BrokenPipeError, ConnectionResetError) as e:
            # Client closed connection before we could send response
            print(f"[http] Client disconnected before response could be sent: {e.__class__.__name__}", file=sys.stderr)
    
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
                    self.send_json(result, 202)
                elif action == "stop":
                    result = controller.stop_container(short_name)
                    self.send_json(result, 202)
                elif action == "restart":
                    result = controller.restart_container(short_name)
                    self.send_json(result, 202)
                elif action == "create-and-start":
                    result = controller.create_and_start_container(short_name)
                    self.send_json(result, 202)
                else:
                    error = {"error": f"Unknown action: {action}"}
                    print(f"[http] ERROR: {error['error']}", file=sys.stderr)
                    self.send_json(error, 404)
                    
            except (BrokenPipeError, ConnectionResetError) as e:
                # Client closed connection - don't try to send error response
                print(f"[http] Client disconnected during {action} operation: {e.__class__.__name__}", file=sys.stderr)
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
            except (BrokenPipeError, ConnectionResetError) as e:
                # Client closed connection - don't try to send error response
                print(f"[http] Client disconnected during list containers: {e.__class__.__name__}", file=sys.stderr)
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
            except (BrokenPipeError, ConnectionResetError) as e:
                # Client closed connection - don't try to send error response
                print(f"[http] Client disconnected during status request: {e.__class__.__name__}", file=sys.stderr)
            except ValueError as e:
                error = {"error": str(e)}
                print(f"[http] ERROR: {error['error']}", file=sys.stderr)
                self.send_json(error, 404)
            except Exception as e:
                error = {"error": str(e)}
                print(f"[http] ERROR: {error['error']}", file=sys.stderr)
                self.send_json(error, 500)
            return
        
        # Container logs endpoint: /container/<name>/logs
        if len(path_parts) == 3 and path_parts[0] == "container" and path_parts[2] == "logs":
            short_name = path_parts[1]
            
            # Parse query parameters for tail parameter
            from urllib.parse import parse_qs
            query_params = parse_qs(parsed.query)
            tail = int(query_params.get('tail', ['500'])[0])
            
            print(f"[http] Logs request for container: {short_name} (tail={tail})")
            
            try:
                result = controller.get_logs(short_name, tail=tail)
                self.send_json(result)
            except (BrokenPipeError, ConnectionResetError) as e:
                # Client closed connection - don't try to send error response
                print(f"[http] Client disconnected during logs request: {e.__class__.__name__}", file=sys.stderr)
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
    print("[http]   POST /container/<name>/start           - Start a container")
    print("[http]   POST /container/<name>/stop            - Stop a container")
    print("[http]   POST /container/<name>/restart         - Restart a container")
    print("[http]   POST /container/<name>/create-and-start - Create and start a container")
    print("[http]   GET  /container/<name>/status          - Get container status")
    print("[http]   GET  /container/<name>/logs?tail=N     - Get container logs")
    print("[http]   GET  /containers                       - List all containers")
    print("[http]   GET  /health                           - Health check")
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