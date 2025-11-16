#!/usr/bin/env python3
"""
CPU Profiling Utilities for Compositor
Provides timing decorators and GStreamer element monitoring
"""
import time
import functools
import threading
from collections import defaultdict

# Thread-safe timing storage
_timing_data = defaultdict(lambda: {"count": 0, "total_time": 0, "max_time": 0})
_timing_lock = threading.Lock()


def profile_timing(func):
    """
    Decorator to measure and log function execution time.
    
    Usage:
        @profile_timing
        def my_function():
            ...
    """
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        start_time = time.perf_counter()
        result = func(*args, **kwargs)
        elapsed = time.perf_counter() - start_time
        
        func_name = func.__name__
        with _timing_lock:
            _timing_data[func_name]["count"] += 1
            _timing_data[func_name]["total_time"] += elapsed
            _timing_data[func_name]["max_time"] = max(
                _timing_data[func_name]["max_time"], elapsed
            )
        
        # Log slow calls (> 10ms)
        if elapsed > 0.010:
            print(f"[profile] {func_name} took {elapsed*1000:.2f}ms (slow)", flush=True)
        
        return result
    return wrapper


def get_timing_stats():
    """Get timing statistics for all profiled functions."""
    with _timing_lock:
        stats = {}
        for func_name, data in _timing_data.items():
            if data["count"] > 0:
                avg_time = data["total_time"] / data["count"]
                stats[func_name] = {
                    "calls": data["count"],
                    "total_ms": data["total_time"] * 1000,
                    "avg_ms": avg_time * 1000,
                    "max_ms": data["max_time"] * 1000
                }
        return stats


def print_timing_report():
    """Print a formatted timing report."""
    stats = get_timing_stats()
    if not stats:
        print("[profile] No timing data collected yet", flush=True)
        return
    
    print("\n" + "="*70, flush=True)
    print("PROFILING REPORT", flush=True)
    print("="*70, flush=True)
    print(f"{'Function':<30} {'Calls':>8} {'Avg(ms)':>10} {'Max(ms)':>10} {'Total(ms)':>10}", flush=True)
    print("-"*70, flush=True)
    
    # Sort by total time descending
    sorted_stats = sorted(stats.items(), key=lambda x: x[1]["total_ms"], reverse=True)
    
    for func_name, data in sorted_stats:
        print(f"{func_name:<30} {data['calls']:>8} {data['avg_ms']:>10.2f} "
              f"{data['max_ms']:>10.2f} {data['total_ms']:>10.2f}", flush=True)
    
    print("="*70 + "\n", flush=True)


def monitor_gst_element_cpu(element, element_name):
    """
    Monitor CPU usage of a GStreamer element.
    
    Args:
        element: GStreamer element to monitor
        element_name: Name for logging
    
    Returns:
        dict with CPU stats if available
    """
    try:
        # Try to get element statistics
        state = element.get_state(0)
        current_state = state[1]
        
        stats = {
            "name": element_name,
            "state": str(current_state),
        }
        
        # Try to get performance properties if available
        try:
            # Some elements expose performance metrics
            if hasattr(element.props, 'stats'):
                element_stats = element.get_property('stats')
                if element_stats:
                    stats["element_stats"] = str(element_stats)
        except:
            pass
        
        return stats
    except Exception as e:
        return {"name": element_name, "error": str(e)}


class CPUMonitor:
    """Monitor CPU usage of compositor components."""
    
    def __init__(self, compositor_manager):
        self.compositor = compositor_manager
        self.running = False
        self.thread = None
    
    def start(self, interval_seconds=10):
        """Start CPU monitoring in background thread."""
        if self.running:
            return
        
        self.running = True
        self.thread = threading.Thread(target=self._monitor_loop, args=(interval_seconds,), daemon=True)
        self.thread.start()
        print(f"[cpu-monitor] Started CPU monitoring (interval={interval_seconds}s)", flush=True)
    
    def stop(self):
        """Stop CPU monitoring."""
        self.running = False
        if self.thread:
            self.thread.join(timeout=2)
    
    def _monitor_loop(self, interval):
        """Background monitoring loop."""
        import psutil
        import os
        
        pid = os.getpid()
        process = psutil.Process(pid)
        
        while self.running:
            try:
                # Get process CPU usage
                cpu_percent = process.cpu_percent(interval=1.0)
                memory_mb = process.memory_info().rss / 1024 / 1024
                num_threads = process.num_threads()
                
                print(f"\n[cpu-monitor] ========================================", flush=True)
                print(f"[cpu-monitor] Process CPU: {cpu_percent:.1f}% | Memory: {memory_mb:.0f}MB | Threads: {num_threads}", flush=True)
                
                # Monitor GStreamer elements
                if self.compositor.output_elements:
                    x264 = self.compositor.output_elements.get('x264enc')
                    if x264:
                        stats = monitor_gst_element_cpu(x264, "x264enc")
                        print(f"[cpu-monitor] x264enc: {stats}", flush=True)
                    
                    comp = self.compositor.output_elements.get('compositor')
                    if comp:
                        stats = monitor_gst_element_cpu(comp, "compositor")
                        print(f"[cpu-monitor] compositor: {stats}", flush=True)
                    
                    mixer = self.compositor.output_elements.get('audiomixer')
                    if mixer:
                        stats = monitor_gst_element_cpu(mixer, "audiomixer")
                        print(f"[cpu-monitor] audiomixer: {stats}", flush=True)
                
                # Print timing stats
                print_timing_report()
                
                # Sleep until next interval
                time.sleep(interval - 1.0)  # -1 because we already slept 1s for cpu_percent
                
            except Exception as e:
                print(f"[cpu-monitor] Error: {e}", flush=True)
                time.sleep(interval)