#!/usr/bin/env python3
import sys
import json
import os
import yaml
import subprocess
import threading
import asyncio
import websockets
import urllib.request
import urllib.error
from datetime import datetime
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
    # Initialize Docker client with timeout to prevent indefinite blocking
    client = docker.from_env(timeout=10)
    print("[startup] ✓ Docker client initialized with 10s timeout", flush=True)
except Exception as e:
    print(f"[startup] ERROR: Failed to initialize Docker client: {e}", file=sys.stderr)
    sys.exit(1)

# Get project name from environment or use default
PROJECT_NAME = os.getenv("COMPOSE_PROJECT_NAME", "relayer")
print(f"[startup] Project name: {PROJECT_NAME}", flush=True)

# Get project path from environment
PROJECT_PATH = os.getenv("PROJECT_PATH", "/app")
COMPOSE_FILE = os.path.join(PROJECT_PATH, "docker-compose.yml")
SHARED_DIR = os.getenv("SHARED_DIR", "/app/shared")
PRIVACY_MODE_FILE = os.path.join(SHARED_DIR, "privacy_mode.json")
MULTIPLEXER_URL = os.getenv("MULTIPLEXER_URL", "http://multiplexer:8091")
print(f"[startup] Project path: {PROJECT_PATH}", flush=True)
print(f"[startup] Compose file: {COMPOSE_FILE}", flush=True)
print(f"[startup] Shared directory: {SHARED_DIR}", flush=True)
print(f"[startup] Privacy mode file: {PRIVACY_MODE_FILE}", flush=True)
print(f"[startup] Multiplexer URL: {MULTIPLEXER_URL}", flush=True)


class ScenePrivacyManager:
    """Manages scene and privacy mode state."""
    
    def __init__(self, privacy_file_path, multiplexer_url):
        self.privacy_file_path = privacy_file_path
        self.multiplexer_url = multiplexer_url
        self._current_scene = "unknown"
        self._scene_timestamp = datetime.utcnow()
        self._privacy_enabled = False
        self._lock = threading.Lock()
        self._change_callbacks = []
        
        # Load privacy state from file
        self._load_privacy_state()
        
        print(f"[scene-privacy] Initialized: scene={self._current_scene}, privacy={self._privacy_enabled}", flush=True)
    
    def _load_privacy_state(self):
        """Load privacy mode state from persistent file."""
        try:
            if os.path.exists(self.privacy_file_path):
                with open(self.privacy_file_path, 'r') as f:
                    data = json.load(f)
                    self._privacy_enabled = data.get('enabled', False)
                    print(f"[scene-privacy] Loaded privacy state from file: {self._privacy_enabled}", flush=True)
            else:
                print(f"[scene-privacy] No privacy file found, defaulting to disabled", flush=True)
        except Exception as e:
            print(f"[scene-privacy] Error loading privacy state: {e}", file=sys.stderr)
            self._privacy_enabled = False
    
    def _save_privacy_state(self):
        """Save privacy mode state to persistent file."""
        try:
            # Ensure directory exists
            os.makedirs(os.path.dirname(self.privacy_file_path), exist_ok=True)
            
            data = {
                'enabled': self._privacy_enabled,
                'updated_at': datetime.utcnow().isoformat() + 'Z'
            }
            with open(self.privacy_file_path, 'w') as f:
                json.dump(data, f, indent=2)
            print(f"[scene-privacy] Saved privacy state to file: {self._privacy_enabled}", flush=True)
        except Exception as e:
            print(f"[scene-privacy] Error saving privacy state: {e}", file=sys.stderr)
    
    def _notify_multiplexer(self, enabled):
        """Notify the multiplexer about privacy mode change via HTTP callback."""
        def do_notify():
            try:
                url = f"{self.multiplexer_url}/privacy"
                data = json.dumps({'enabled': enabled}).encode('utf-8')
                req = urllib.request.Request(
                    url,
                    data=data,
                    headers={'Content-Type': 'application/json'},
                    method='POST'
                )
                
                with urllib.request.urlopen(req, timeout=5) as response:
                    result = response.read().decode('utf-8')
                    print(f"[scene-privacy] Notified multiplexer of privacy change: {result}", flush=True)
                    
            except urllib.error.URLError as e:
                print(f"[scene-privacy] Failed to notify multiplexer (may not be running): {e}", file=sys.stderr)
            except Exception as e:
                print(f"[scene-privacy] Error notifying multiplexer: {e}", file=sys.stderr)
        
        # Run in background thread to not block
        thread = threading.Thread(target=do_notify, daemon=True)
        thread.start()
    
    def register_change_callback(self, callback):
        """Register a callback to be called when scene or privacy changes."""
        self._change_callbacks.append(callback)
    
    def _trigger_change_callbacks(self, change_type, data):
        """Trigger all registered callbacks."""
        print(f"[scene_change_debug] _trigger_change_callbacks: type={change_type}, callbacks={len(self._change_callbacks)}", flush=True)
        for i, callback in enumerate(self._change_callbacks):
            try:
                print(f"[scene_change_debug] Calling callback {i+1}/{len(self._change_callbacks)}", flush=True)
                callback(change_type, data)
            except Exception as e:
                print(f"[scene-privacy] Error in change callback: {e}", file=sys.stderr)
    
    @property
    def current_scene(self):
        with self._lock:
            return self._current_scene
    
    @property
    def scene_timestamp(self):
        with self._lock:
            return self._scene_timestamp
    
    @property
    def privacy_enabled(self):
        with self._lock:
            return self._privacy_enabled
    
    def set_scene(self, scene):
        """Set the current scene (called by multiplexer)."""
        with self._lock:
            old_scene = self._current_scene
            print(f"[scene_change_debug] set_scene called: old={old_scene}, new={scene}", flush=True)
            if old_scene != scene:
                self._current_scene = scene
                self._scene_timestamp = datetime.utcnow()
                print(f"[scene-privacy] Scene changed: {old_scene} -> {scene}", flush=True)
                print(f"[scene_change_debug] Triggering callbacks with type='scene_change'", flush=True)
                
                # Trigger callbacks outside the lock
                self._trigger_change_callbacks('scene_change', {
                    'previous_scene': old_scene,
                    'current_scene': scene,
                    'timestamp': self._scene_timestamp.isoformat() + 'Z'
                })
                return True
            else:
                print(f"[scene_change_debug] Scene unchanged, skipping callbacks", flush=True)
            return False
    
    def enable_privacy(self):
        """Enable privacy mode."""
        with self._lock:
            if not self._privacy_enabled:
                self._privacy_enabled = True
                print(f"[scene-privacy] Privacy mode ENABLED", flush=True)
                self._save_privacy_state()
                
                # Notify multiplexer in background
                self._notify_multiplexer(True)
                
                # Trigger callbacks
                self._trigger_change_callbacks('privacy_change', {
                    'enabled': True,
                    'timestamp': datetime.utcnow().isoformat() + 'Z'
                })
                return True
            return False
    
    def disable_privacy(self):
        """Disable privacy mode."""
        with self._lock:
            if self._privacy_enabled:
                self._privacy_enabled = False
                print(f"[scene-privacy] Privacy mode DISABLED", flush=True)
                self._save_privacy_state()
                
                # Notify multiplexer in background
                self._notify_multiplexer(False)
                
                # Trigger callbacks
                self._trigger_change_callbacks('privacy_change', {
                    'enabled': False,
                    'timestamp': datetime.utcnow().isoformat() + 'Z'
                })
                return True
            return False
    
    def get_state(self):
        """Get the current state as a dictionary."""
        with self._lock:
            return {
                'current_scene': self._current_scene,
                'scene_timestamp': self._scene_timestamp.isoformat() + 'Z',
                'privacy_enabled': self._privacy_enabled
            }


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
        """Format detailed status string from container attributes.
        Returns tuple: (status_detail_string, started_at_iso, finished_at_iso)
        """
        try:
            state = container.attrs['State']
            status = state.get('Status', 'unknown')
            started_at_raw = state.get('StartedAt', '')
            finished_at_raw = state.get('FinishedAt', '')
            
            # Filter out invalid/zero timestamps (Docker returns 0001-01-01T00:00:00Z for unset times)
            def is_valid_timestamp(ts):
                if not ts or ts == '':
                    return False
                # Check if timestamp is the zero time (year 0001 or 1970 epoch)
                if ts.startswith('0001-') or ts.startswith('0000-'):
                    return False
                return True
            
            started_at = started_at_raw if is_valid_timestamp(started_at_raw) else None
            finished_at = finished_at_raw if is_valid_timestamp(finished_at_raw) else None
            
            # For not-running containers
            if status == 'exited':
                exit_code = state.get('ExitCode', 0)
                if finished_at:
                    from datetime import datetime
                    try:
                        finished = datetime.fromisoformat(finished_at.replace('Z', '+00:00'))
                        now = datetime.now(finished.tzinfo)
                        delta = now - finished
                        time_ago = self._format_time_delta(delta)
                        return (f"Exited ({exit_code}) {time_ago} ago", started_at, finished_at)
                    except:
                        return (f"Exited ({exit_code})", started_at, finished_at)
                return (f"Exited ({exit_code})", started_at, finished_at)
            
            # For running containers
            elif status == 'running':
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
                            return (f"Up {uptime} (healthy)", started_at, finished_at)
                        elif health_status == 'unhealthy':
                            return (f"Up {uptime} (unhealthy)", started_at, finished_at)
                        elif health_status == 'starting':
                            return (f"Up {uptime} (health: starting)", started_at, finished_at)
                        else:
                            return (f"Up {uptime}", started_at, finished_at)
                    except:
                        return ("Up", started_at, finished_at)
                return ("Up", started_at, finished_at)
            
            # For other statuses
            elif status == 'created':
                return ("Created", started_at, finished_at)
            elif status == 'paused':
                return ("Paused", started_at, finished_at)
            elif status == 'restarting':
                return ("Restarting", started_at, finished_at)
            else:
                return (status.capitalize(), started_at, finished_at)
                
        except Exception as e:
            print(f"[controller] WARNING: Failed to format detailed status: {e}", file=sys.stderr)
            return ("Unknown", None, None)
    
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
        """Convert short name to full container name using compose parser."""
        # Look up actual name from compose parser first
        service_info = self.compose_parser.get_service(short_name)
        if service_info:
            return service_info['full_name']
        # Fallback to prefix pattern for containers not in compose file
        return f"{self.project_name}-{short_name}"
    
    def _is_recreation_error(self, error_text):
        """Check if error text indicates a failure that requires container recreation.
        
        This includes:
        - Network errors (stale network IDs)
        - Mount/volume errors (stale overlay filesystem)
        - OCI runtime errors related to container setup
        """
        if not error_text:
            return False
        
        error_lower = error_text.lower()
        
        # Network-related errors that require recreation
        network_patterns = [
            ("network", "not found"),
            ("failed to set up container networking",),
            ("error response from daemon", "network")
        ]
        
        # Mount/volume-related errors that require recreation
        mount_patterns = [
            ("error mounting",),
            ("failed to create task for container",),
            ("error during container init",),
            ("not a directory", "mount"),
            ("are you trying to mount a directory onto a file",),
            ("oci runtime create failed",),
            ("unable to start container process",)
        ]
        
        # Check all error patterns
        all_patterns = network_patterns + mount_patterns
        
        for pattern in all_patterns:
            if all(phrase in error_lower for phrase in pattern):
                return True
        
        return False
    
    def _remove_container_sync(self, short_name):
        """Remove a container using docker compose (synchronous)."""
        container_name = self.get_container_name(short_name)
        
        try:
            # Get service info from compose parser
            service_info = self.compose_parser.get_service(short_name)
            if not service_info:
                error_msg = f"Service not found in compose file: {short_name}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return False
            
            service_name = service_info['service_name']
            
            # Use docker compose rm command with force and stop
            env_file = os.path.join(PROJECT_PATH, ".env")
            cmd = ["docker", "compose", "--project-directory", PROJECT_PATH,
                   "-f", COMPOSE_FILE, "--env-file", env_file,
                   "rm", "-f", "-s", service_name]
            
            print(f"[controller] Executing: {' '.join(cmd)}")
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=60
            )
            
            if result.returncode != 0:
                error_msg = f"Failed to remove container: {result.stderr}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return False
            
            print(f"[controller] ✓ Container {container_name} removed successfully")
            if result.stdout.strip():
                print(f"[controller] Output: {result.stdout}")
            
            return True
            
        except subprocess.TimeoutExpired:
            error_msg = f"Timeout removing container: {short_name}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            return False
        except Exception as e:
            print(f"[controller] ERROR removing container: {str(e)}", file=sys.stderr)
            return False
    
    def _start_container_sync(self, short_name):
        """Synchronous start operation using docker compose (run in background thread)."""
        container_name = self.get_container_name(short_name)
        
        try:
            # Check if container exists first
            try:
                container = self.client.containers.get(container_name)
                container.reload()
                if container.status == "running":
                    print(f"[controller] Container {container_name} is already running")
                    return
            except docker.errors.NotFound:
                # Container doesn't exist, create and start it instead
                print(f"[controller] Container {container_name} not found, creating and starting...")
                self._create_and_start_container_sync(short_name)
                return
            
            # Get service info from compose parser
            service_info = self.compose_parser.get_service(short_name)
            if not service_info:
                error_msg = f"Service not found in compose file: {short_name}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return
            
            service_name = service_info['service_name']
            
            # Use docker compose start command
            env_file = os.path.join(PROJECT_PATH, ".env")
            cmd = ["docker", "compose", "--project-directory", PROJECT_PATH,
                   "-f", COMPOSE_FILE, "--env-file", env_file,
                   "start", service_name]
            
            print(f"[controller] Executing: {' '.join(cmd)}")
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=60
            )
            
            if result.returncode != 0:
                error_msg = f"Failed to start container: {result.stderr}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                
                # Check if this is an error that requires container recreation
                if self._is_recreation_error(result.stderr):
                    print(f"[controller] ⚠️  Container error detected for {container_name}", flush=True)
                    print(f"[controller] 🔄 Attempting automatic recreation to fix configuration...", flush=True)
                    
                    # Remove the container with stale configuration
                    if self._remove_container_sync(short_name):
                        print(f"[controller] 🔨 Recreating container with current configuration...", flush=True)
                        # Recreate using the existing method
                        self._create_and_start_container_sync(short_name)
                    else:
                        print(f"[controller] ❌ Failed to remove container for recreation", file=sys.stderr)
                
                return
            
            print(f"[controller] ✓ Container {container_name} started successfully")
            if result.stdout.strip():
                print(f"[controller] Output: {result.stdout}")
            
        except subprocess.TimeoutExpired:
            error_msg = f"Timeout starting container: {short_name}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
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
        """Synchronous stop operation using docker compose (run in background thread)."""
        container_name = self.get_container_name(short_name)
        
        try:
            # Check if container exists and is running
            try:
                container = self.client.containers.get(container_name)
                container.reload()
                if container.status != "running":
                    print(f"[controller] Container {container_name} is already stopped")
                    return
            except docker.errors.NotFound:
                error_msg = f"Container not found: {short_name}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return
            
            # Get service info from compose parser
            service_info = self.compose_parser.get_service(short_name)
            if not service_info:
                error_msg = f"Service not found in compose file: {short_name}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return
            
            service_name = service_info['service_name']
            
            # Use docker compose stop command with 30 second timeout
            env_file = os.path.join(PROJECT_PATH, ".env")
            cmd = ["docker", "compose", "--project-directory", PROJECT_PATH,
                   "-f", COMPOSE_FILE, "--env-file", env_file,
                   "stop", "-t", "30", service_name]
            
            print(f"[controller] Executing: {' '.join(cmd)}")
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=60
            )
            
            if result.returncode != 0:
                error_msg = f"Failed to stop container: {result.stderr}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return
            
            print(f"[controller] ✓ Container {container_name} stopped successfully")
            if result.stdout.strip():
                print(f"[controller] Output: {result.stdout}")
            
        except subprocess.TimeoutExpired:
            error_msg = f"Timeout stopping container: {short_name}"
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
        """Synchronous restart operation using docker compose (run in background thread)."""
        container_name = self.get_container_name(short_name)
        
        try:
            # Check if container exists
            try:
                container = self.client.containers.get(container_name)
            except docker.errors.NotFound:
                error_msg = f"Container not found: {short_name}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return
            
            # Get service info from compose parser
            service_info = self.compose_parser.get_service(short_name)
            if not service_info:
                error_msg = f"Service not found in compose file: {short_name}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return
            
            service_name = service_info['service_name']
            
            # Use docker compose restart command with 30 second timeout
            env_file = os.path.join(PROJECT_PATH, ".env")
            cmd = ["docker", "compose", "--project-directory", PROJECT_PATH,
                   "-f", COMPOSE_FILE, "--env-file", env_file,
                   "restart", "-t", "30", service_name]
            
            print(f"[controller] Executing: {' '.join(cmd)}")
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=60
            )
            
            if result.returncode != 0:
                error_msg = f"Failed to restart container: {result.stderr}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                return
            
            print(f"[controller] ✓ Container {container_name} restarted successfully")
            if result.stdout.strip():
                print(f"[controller] Output: {result.stdout}")
            
        except subprocess.TimeoutExpired:
            error_msg = f"Timeout restarting container: {short_name}"
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
            
            detailed_status, started_at, finished_at = self._format_detailed_status(container)
            health_status = self._get_health_status(container)
            
            status_info = {
                "container": short_name,
                "status": container.status,
                "status_detail": detailed_status,
                "running": container.status == "running",
                "health": health_status,
                "id": container.short_id,
                "startedAt": started_at,
                "finishedAt": finished_at
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
        import time
        start_time = time.time()
        
        print("[controller] Starting list_containers operation", flush=True)
        
        try:
            # Get all compose services to build lookup map
            compose_services = self.compose_parser.get_services()
            full_name_to_short = {svc['full_name']: svc['name'] for svc in compose_services.values()}
            
            # Get all actual containers from Docker with timing
            print("[controller] Querying Docker API for container list...", flush=True)
            docker_start = time.time()
            
            try:
                all_containers = self.client.containers.list(all=True)
                docker_duration = time.time() - docker_start
                print(f"[controller] Docker API returned {len(all_containers)} containers in {docker_duration:.3f}s", flush=True)
            except docker.errors.DockerException as docker_err:
                docker_duration = time.time() - docker_start
                error_msg = f"Docker API timeout or error after {docker_duration:.3f}s: {str(docker_err)}"
                print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
                # Return compose services with 'unknown' status as fallback
                print("[controller] Falling back to compose services with unknown status", file=sys.stderr)
                fallback_containers = []
                for short_name, service_info in compose_services.items():
                    fallback_containers.append({
                        "name": short_name,
                        "full_name": service_info['full_name'],
                        "status": "unknown",
                        "status_detail": "Docker API unavailable",
                        "running": False,
                        "health": None,
                        "id": None,
                        "created": False,
                        "is_manual": service_info.get('is_manual', False)
                    })
                return {"containers": fallback_containers, "warning": "Docker API timeout - showing incomplete data"}
            
            containers_by_name = {}
            
            for container in all_containers:
                # Match container by full name from compose services
                if container.name in full_name_to_short:
                    short_name = full_name_to_short[container.name]
                    detailed_status, started_at, finished_at = self._format_detailed_status(container)
                    health_status = self._get_health_status(container)
                    
                    containers_by_name[short_name] = {
                        "name": short_name,
                        "full_name": container.name,
                        "status": container.status,
                        "status_detail": detailed_status,
                        "running": container.status == "running",
                        "health": health_status,
                        "id": container.short_id,
                        "created": True,
                        "startedAt": started_at,
                        "finishedAt": finished_at
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
                        "is_manual": service_info.get('is_manual', False),
                        "startedAt": None,
                        "finishedAt": None
                    })
            
            total_duration = time.time() - start_time
            print(f"[controller] Listed {len(merged_containers)} services/containers in {total_duration:.3f}s", flush=True)
            return {"containers": merged_containers}
            
        except docker.errors.APIError as e:
            total_duration = time.time() - start_time
            error_msg = f"Docker API error after {total_duration:.3f}s: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            raise RuntimeError(error_msg)
        except Exception as e:
            total_duration = time.time() - start_time
            error_msg = f"Unexpected error after {total_duration:.3f}s: {str(e)}"
            print(f"[controller] ERROR: {error_msg}", file=sys.stderr)
            import traceback
            traceback.print_exc()
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
            
            # Use project path from environment - same path as on host
            env_file = os.path.join(PROJECT_PATH, ".env")
            
            # Build command - add --no-deps for manual profile services to avoid recreating dependencies
            cmd = ["docker", "compose", "--project-directory", PROJECT_PATH, "-f", COMPOSE_FILE, "--env-file", env_file, "up", "-d", "--remove-orphans"]
            
            # For manual profile services, use --no-deps to prevent dependency resolution
            if service_info.get('is_manual', False):
                cmd.append("--no-deps")
                print(f"[controller] Service {short_name} has manual profile - using --no-deps to avoid affecting dependencies")
            
            cmd.append(service_name)
            
            # Diagnostic logging
            print(f"[controller] Executing: {' '.join(cmd)}")
            print(f"[controller] DEBUG: Project directory: {PROJECT_PATH}")
            shared_path = os.path.join(PROJECT_PATH, "shared")
            offline_image_path = os.path.join(PROJECT_PATH, "offline-image")
            print(f"[controller] DEBUG: {shared_path} exists: {os.path.exists(shared_path)}")
            print(f"[controller] DEBUG: {offline_image_path} exists: {os.path.exists(offline_image_path)}")
            if os.path.exists(shared_path):
                print(f"[controller] DEBUG: Contents of shared: {os.listdir(shared_path)}")
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
            
            # DEBUG: Inspect the created container's mounts
            try:
                container = self.client.containers.get(self.get_container_name(short_name))
                mounts = container.attrs.get('Mounts', [])
                print(f"[controller] DEBUG: Container {short_name} has {len(mounts)} mounts:")
                for mount in mounts:
                    mount_type = mount.get('Type', 'unknown')
                    source = mount.get('Source', 'unknown')
                    dest = mount.get('Destination', 'unknown')
                    print(f"[controller] DEBUG:   {mount_type}: {source} -> {dest}")
            except Exception as e:
                print(f"[controller] DEBUG: Failed to inspect mounts: {e}")
            
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

# Initialize scene and privacy manager
print("[startup] Initializing scene and privacy manager...", flush=True)
scene_privacy_manager = ScenePrivacyManager(PRIVACY_MODE_FILE, MULTIPLEXER_URL)


class WebSocketServer:
    """WebSocket server for real-time container status and log streaming."""
    
    def __init__(self, controller, scene_privacy_manager):
        self.controller = controller
        self.scene_privacy_manager = scene_privacy_manager
        self.clients = set()
        self.log_subscribers = {}  # {container_name: {client: last_timestamp}}
        self.last_container_state = {}
        self.last_sent_logs = {}  # {container_name: last_log_line} - tracks last sent log per container
        self.monitoring_task = None
        self.loop = None  # Event loop reference for cross-thread access
        
        # Register for scene/privacy change notifications
        self.scene_privacy_manager.register_change_callback(self._on_scene_privacy_change)
    
    def _on_scene_privacy_change(self, change_type, data):
        """Handle scene or privacy change events."""
        print(f"[scene_change_debug] _on_scene_privacy_change: type={change_type}, data={data}", flush=True)
        # Schedule broadcast in the event loop using stored reference
        try:
            if self.loop is not None:
                print(f"[scene_change_debug] Scheduling WebSocket broadcast for type={change_type} using stored loop", flush=True)
                asyncio.run_coroutine_threadsafe(
                    self._broadcast_scene_privacy_change(change_type, data),
                    self.loop
                )
            else:
                print(f"[scene_change_debug] WARNING: Event loop not stored yet!", flush=True)
        except Exception as e:
            print(f"[ws] Error scheduling scene/privacy broadcast: {e}", file=sys.stderr)
            print(f"[scene_change_debug] Exception in _on_scene_privacy_change: {e}", flush=True)
    
    async def _broadcast_scene_privacy_change(self, change_type, data):
        """Broadcast scene or privacy change to all clients."""
        print(f"[scene_change_debug] _broadcast_scene_privacy_change: type={change_type}, clients={len(self.clients)}", flush=True)
        if not self.clients:
            print(f"[scene_change_debug] No clients connected, skipping broadcast", flush=True)
            return
        
        state = self.scene_privacy_manager.get_state()
        message = {
            'type': change_type,
            'timestamp': datetime.utcnow().isoformat() + 'Z',
            'current_scene': state['current_scene'],
            'privacy_enabled': state['privacy_enabled'],
            'change_data': data
        }
        
        message_json = json.dumps(message)
        print(f"[scene_change_debug] Broadcasting message: {message_json}", flush=True)
        disconnected = set()
        
        for client in self.clients:
            try:
                await client.send(message_json)
                print(f"[scene_change_debug] Sent to client {id(client)}", flush=True)
            except Exception as e:
                print(f"[ws] Error broadcasting scene/privacy change to client: {e}", file=sys.stderr)
                disconnected.add(client)
        
        # Clean up disconnected clients
        for client in disconnected:
            await self.unregister_client(client)
        
        print(f"[ws] Broadcasted {change_type} to {len(self.clients) - len(disconnected)} client(s)", flush=True)
        print(f"[scene_change_debug] Broadcast complete", flush=True)
        
    async def register_client(self, websocket):
        """Register a new WebSocket client."""
        self.clients.add(websocket)
        client_id = id(websocket)
        print(f"[ws] Client connected (id={client_id}), total clients: {len(self.clients)}", flush=True)
        
        # Send initial state
        try:
            await self.send_initial_state(websocket)
        except Exception as e:
            print(f"[ws] Error sending initial state to client {client_id}: {e}", file=sys.stderr)
    
    async def unregister_client(self, websocket):
        """Unregister a WebSocket client and clean up subscriptions."""
        self.clients.discard(websocket)
        client_id = id(websocket)
        
        # Remove from all log subscriptions
        for container_name in list(self.log_subscribers.keys()):
            if websocket in self.log_subscribers[container_name]:
                del self.log_subscribers[container_name][websocket]
                if not self.log_subscribers[container_name]:
                    del self.log_subscribers[container_name]
        
        print(f"[ws] Client disconnected (id={client_id}), remaining clients: {len(self.clients)}", flush=True)
    
    async def send_initial_state(self, websocket):
        """Send initial container state to a newly connected client."""
        client_id = id(websocket)
        print(f"[ws] send_initial_state starting for client {client_id}", flush=True)
        try:
            print(f"[ws] Calling list_containers for client {client_id}", flush=True)
            result = self.controller.list_containers()
            containers = result.get('containers', [])
            print(f"[ws] Got {len(containers)} containers for client {client_id}", flush=True)
            
            # Get scene and privacy state
            scene_privacy_state = self.scene_privacy_manager.get_state()
            
            message = {
                'type': 'initial_state',
                'timestamp': datetime.utcnow().isoformat() + 'Z',
                'containers': containers,
                'current_scene': scene_privacy_state['current_scene'],
                'privacy_enabled': scene_privacy_state['privacy_enabled'],
                'scene_timestamp': scene_privacy_state['scene_timestamp']
            }
            
            print(f"[ws] Sending initial_state message to client {client_id}", flush=True)
            await websocket.send(json.dumps(message))
            print(f"[ws] Successfully sent initial state with {len(containers)} containers, scene={scene_privacy_state['current_scene']}, privacy={scene_privacy_state['privacy_enabled']} to client {client_id}", flush=True)
            
        except Exception as e:
            print(f"[ws] Error in send_initial_state for client {client_id}: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
    
    async def broadcast_status_change(self, changes):
        """Broadcast container status changes to all connected clients."""
        if not changes or not self.clients:
            return
        
        # Include current scene/privacy state in every status_change as fallback
        state = self.scene_privacy_manager.get_state()
        
        message = {
            'type': 'status_change',
            'timestamp': datetime.utcnow().isoformat() + 'Z',
            'changes': changes,
            'current_scene': state['current_scene'],
            'privacy_enabled': state['privacy_enabled']
        }
        
        message_json = json.dumps(message)
        disconnected = set()
        
        for client in self.clients:
            try:
                await client.send(message_json)
            except Exception as e:
                print(f"[ws] Error broadcasting to client: {e}", file=sys.stderr)
                disconnected.add(client)
        
        # Clean up disconnected clients
        for client in disconnected:
            await self.unregister_client(client)
    
    async def monitor_container_status(self):
        """Monitor container status every 2 seconds and broadcast changes."""
        print("[ws] Starting container status monitoring loop", flush=True)
        
        while True:
            try:
                # Get current container states
                result = self.controller.list_containers()
                containers = result.get('containers', [])
                
                changes = []
                current_state = {}
                
                for container in containers:
                    name = container['name']
                    current_state[name] = {
                        'status': container['status'],
                        'health': container.get('health'),
                        'status_detail': container.get('status_detail'),
                        'running': container.get('running', False)
                    }
                    
                    # Check if this container state changed
                    if name in self.last_container_state:
                        prev = self.last_container_state[name]
                        curr = current_state[name]
                        
                        if (prev['status'] != curr['status'] or
                            prev['health'] != curr['health'] or
                            prev['running'] != curr['running']):
                            changes.append({
                                'name': name,
                                'previousStatus': prev['status'],
                                'previousHealth': prev.get('health'),
                                'currentStatus': curr['status'],
                                'currentHealth': curr.get('health'),
                                'running': curr['running'],
                                'statusDetail': curr['status_detail']
                            })
                            print(f"[ws] Status change detected for {name}: {prev['status']}→{curr['status']}", flush=True)
                    else:
                        # New container appeared
                        if container.get('created', True):  # Only report if actually created
                            changes.append({
                                'name': name,
                                'previousStatus': None,
                                'previousHealth': None,
                                'currentStatus': current_state[name]['status'],
                                'currentHealth': current_state[name]['health'],
                                'running': current_state[name]['running'],
                                'statusDetail': current_state[name]['status_detail']
                            })
                            print(f"[ws] New container detected: {name}", flush=True)
                
                # Update stored state
                self.last_container_state = current_state
                
                # Broadcast changes if any
                if changes and self.clients:
                    await self.broadcast_status_change(changes)
                
            except Exception as e:
                print(f"[ws] Error in monitoring loop: {e}", file=sys.stderr)
                import traceback
                traceback.print_exc()
            
            # Wait 2 seconds before next check
            await asyncio.sleep(2)
    
    async def handle_log_subscription(self, websocket, container_name, lines=100):
        """Handle log subscription request from a client."""
        try:
            # Send initial log snapshot
            result = self.controller.get_logs(container_name, tail=lines)
            logs = result.get('logs', [])
            
            message = {
                'type': 'log_snapshot',
                'container': container_name,
                'logs': logs,
                'lastLogTimestamp': datetime.utcnow().isoformat() + 'Z'
            }
            
            await websocket.send(json.dumps(message))
            
            # Register subscriber
            if container_name not in self.log_subscribers:
                self.log_subscribers[container_name] = {}
            
            self.log_subscribers[container_name][websocket] = datetime.utcnow()
            
            print(f"[ws] Client subscribed to logs for {container_name}", flush=True)
            
        except Exception as e:
            print(f"[ws] Error handling log subscription for {container_name}: {e}", file=sys.stderr)
    
    async def handle_log_unsubscription(self, websocket, container_name):
        """Handle log unsubscription request from a client."""
        if container_name in self.log_subscribers:
            if websocket in self.log_subscribers[container_name]:
                del self.log_subscribers[container_name][websocket]
                if not self.log_subscribers[container_name]:
                    del self.log_subscribers[container_name]
                print(f"[ws] Client unsubscribed from logs for {container_name}", flush=True)
    
    async def stream_logs(self):
        """Continuously stream new logs to subscribed clients."""
        print("[ws] Starting log streaming loop", flush=True)
        
        while True:
            try:
                if self.log_subscribers:
                    for container_name, subscribers in list(self.log_subscribers.items()):
                        if not subscribers:
                            continue
                        
                        # Get logs for this container
                        try:
                            result = self.controller.get_logs(container_name, tail=50)
                            logs = result.get('logs', [])
                            
                            if logs:
                                # Check if there are actually new logs since last send
                                last_sent = self.last_sent_logs.get(container_name)
                                
                                if last_sent is not None:
                                    # Find the index of the last sent log in current logs
                                    try:
                                        last_sent_idx = logs.index(last_sent)
                                        # Only get logs after the last sent one
                                        new_logs = logs[last_sent_idx + 1:]
                                    except ValueError:
                                        # Last sent log not found (log rotation or container restart)
                                        # Send all logs as they may all be new
                                        new_logs = logs
                                else:
                                    # First time sending logs for this container
                                    new_logs = logs
                                
                                # Only send if there are actually new logs
                                if new_logs:
                                    # Update the last sent log marker
                                    self.last_sent_logs[container_name] = logs[-1]
                                    
                                    disconnected = set()
                                    
                                    for client, last_timestamp in list(subscribers.items()):
                                        try:
                                            message = {
                                                'type': 'new_logs',
                                                'container': container_name,
                                                'logs': new_logs,
                                                'lastLogTimestamp': datetime.utcnow().isoformat() + 'Z'
                                            }
                                            
                                            await client.send(json.dumps(message))
                                            subscribers[client] = datetime.utcnow()
                                            
                                        except Exception as e:
                                            print(f"[ws] Error streaming logs to client: {e}", file=sys.stderr)
                                            disconnected.add(client)
                                    
                                    # Clean up disconnected clients
                                    for client in disconnected:
                                        if client in subscribers:
                                            del subscribers[client]
                                
                        except Exception as e:
                            print(f"[ws] Error getting logs for {container_name}: {e}", file=sys.stderr)
                
            except Exception as e:
                print(f"[ws] Error in log streaming loop: {e}", file=sys.stderr)
            
            # Check for new logs every 1 second
            await asyncio.sleep(1)
    
    async def handle_client_message(self, websocket, message_text):
        """Handle incoming messages from clients."""
        client_id = id(websocket)
        try:
            message = json.loads(message_text)
            msg_type = message.get('type')
            print(f"[ws] Client {client_id} message type: {msg_type}", flush=True)
            
            if msg_type == 'subscribe_logs':
                container = message.get('container')
                lines = message.get('lines', 100)
                print(f"[ws] Client {client_id} subscribing to logs for {container}", flush=True)
                await self.handle_log_subscription(websocket, container, lines)
                
            elif msg_type == 'unsubscribe_logs':
                container = message.get('container')
                print(f"[ws] Client {client_id} unsubscribing from logs for {container}", flush=True)
                await self.handle_log_unsubscription(websocket, container)
                
            else:
                print(f"[ws] Unknown message type from client {client_id}: {msg_type}", file=sys.stderr)
                
        except json.JSONDecodeError as e:
            print(f"[ws] Invalid JSON from client {client_id}: {e}", file=sys.stderr)
        except Exception as e:
            print(f"[ws] Error handling client {client_id} message: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
    
    async def handle_connection(self, websocket):
        """Handle a WebSocket connection."""
        client_id = id(websocket)
        print(f"[ws] handle_connection started for client {client_id}", flush=True)
        
        await self.register_client(websocket)
        print(f"[ws] Client {client_id} registered, entering message loop", flush=True)
        
        try:
            async for message in websocket:
                print(f"[ws] Client {client_id} sent message: {message[:100]}...", flush=True)
                await self.handle_client_message(websocket, message)
        except websockets.exceptions.ConnectionClosed as e:
            print(f"[ws] Client {client_id} connection closed normally: code={e.code}, reason={e.reason}", flush=True)
        except Exception as e:
            print(f"[ws] Error in connection handler for client {client_id}: {e}", file=sys.stderr)
            import traceback
            traceback.print_exc()
        finally:
            print(f"[ws] Cleaning up client {client_id}", flush=True)
            await self.unregister_client(websocket)
            print(f"[ws] Client {client_id} cleanup complete", flush=True)
    
    async def start(self):
        """Start the WebSocket server and monitoring tasks."""
        # Store event loop reference for cross-thread access (e.g., from HTTP thread)
        self.loop = asyncio.get_running_loop()
        print(f"[ws] Stored event loop reference for cross-thread access", flush=True)
        
        # Start monitoring task
        self.monitoring_task = asyncio.create_task(self.monitor_container_status())
        
        # Start log streaming task
        asyncio.create_task(self.stream_logs())
        
        # Start WebSocket server
        print("[ws] Starting WebSocket server on 0.0.0.0:8090", flush=True)
        async with websockets.serve(self.handle_connection, "0.0.0.0", 8090):
            await asyncio.Future()  # Run forever


# Global WebSocket server instance
ws_server = WebSocketServer(controller, scene_privacy_manager)


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
    
    def _read_request_body(self):
        """Read and parse JSON request body."""
        try:
            content_length = int(self.headers.get('Content-Length', 0))
            if content_length > 0:
                body = self.rfile.read(content_length)
                return json.loads(body.decode('utf-8'))
        except Exception as e:
            print(f"[http] Error reading request body: {e}", file=sys.stderr)
        return {}
    
    def do_POST(self):
        """Handle POST requests for container operations."""
        parsed = urlparse(self.path)
        path_parts = parsed.path.strip("/").split("/")
        
        # Scene notification from multiplexer: POST /scene/live or /scene/fallback
        if len(path_parts) == 2 and path_parts[0] == "scene":
            scene = path_parts[1].upper()
            if scene in ["LIVE", "FALLBACK"]:
                print(f"[http] Scene notification from multiplexer: {scene}")
                print(f"[scene_change_debug] HTTP received POST /scene/{scene}", flush=True)
                changed = scene_privacy_manager.set_scene(scene)
                print(f"[scene_change_debug] set_scene returned changed={changed}", flush=True)
                self.send_json({
                    "status": "ok",
                    "scene": scene,
                    "changed": changed
                })
            else:
                self.send_json({"error": f"Invalid scene: {scene}"}, 400)
            return
        
        # Privacy mode control: POST /privacy/enable or /privacy/disable
        if len(path_parts) == 2 and path_parts[0] == "privacy":
            action = path_parts[1].lower()
            if action == "enable":
                print(f"[http] Privacy mode enable request")
                changed = scene_privacy_manager.enable_privacy()
                self.send_json({
                    "status": "ok",
                    "privacy_enabled": True,
                    "changed": changed
                })
            elif action == "disable":
                print(f"[http] Privacy mode disable request")
                changed = scene_privacy_manager.disable_privacy()
                self.send_json({
                    "status": "ok",
                    "privacy_enabled": False,
                    "changed": changed
                })
            else:
                self.send_json({"error": f"Invalid privacy action: {action}"}, 400)
            return
        
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
        
        # Scene state endpoint: GET /scene
        if parsed.path == "/scene":
            print("[http] Scene state request")
            state = scene_privacy_manager.get_state()
            self.send_json({
                "current_scene": state['current_scene'],
                "scene_timestamp": state['scene_timestamp']
            })
            return
        
        # Privacy state endpoint: GET /privacy
        if parsed.path == "/privacy":
            print("[http] Privacy state request")
            state = scene_privacy_manager.get_state()
            self.send_json({
                "privacy_enabled": state['privacy_enabled']
            })
            return
        
        # Combined state endpoint: GET /state
        if parsed.path == "/state":
            print("[http] Full state request")
            self.send_json(scene_privacy_manager.get_state())
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
    print("[http]   POST /scene/live                       - Notify scene change to LIVE")
    print("[http]   POST /scene/fallback                   - Notify scene change to FALLBACK")
    print("[http]   GET  /scene                            - Get current scene")
    print("[http]   POST /privacy/enable                   - Enable privacy mode")
    print("[http]   POST /privacy/disable                  - Disable privacy mode")
    print("[http]   GET  /privacy                          - Get privacy mode state")
    print("[http]   GET  /state                            - Get full state (scene + privacy)")
    print("[http] Ready to accept requests")
    srv.serve_forever()


async def run_websocket():
    """Start the WebSocket server."""
    await ws_server.start()


def run_http_thread():
    """Run HTTP server in a separate thread."""
    run_http()


if __name__ == "__main__":
    print("[main] Starting container controller...")
    
    try:
        # Start HTTP server in a separate thread
        http_thread = threading.Thread(target=run_http_thread, daemon=True)
        http_thread.start()
        print("[main] HTTP server thread started")
        
        # Run WebSocket server in main thread (asyncio event loop)
        asyncio.run(run_websocket())
        
    except KeyboardInterrupt:
        print("[main] Received interrupt signal")
    except Exception as e:
        print(f"[main] ERROR: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
    finally:
        print("[main] Shutdown complete")