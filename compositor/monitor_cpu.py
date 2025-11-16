#!/usr/bin/env python3
"""
Standalone CPU Monitoring Script for Compositor Container
Run this inside the compositor container to monitor real-time CPU usage
"""
import psutil
import time
import sys
import os
import argparse


def monitor_compositor_cpu(interval=2, duration=None):
    """
    Monitor CPU usage of the compositor process.
    
    Args:
        interval: Seconds between measurements
        duration: Total duration to monitor (None = infinite)
    """
    print("="*80)
    print("COMPOSITOR CPU MONITOR")
    print("="*80)
    print(f"Monitoring interval: {interval}s")
    if duration:
        print(f"Duration: {duration}s")
    else:
        print("Duration: Infinite (press Ctrl+C to stop)")
    print("="*80)
    print()
    
    # Find compositor process
    compositor_procs = []
    for proc in psutil.process_iter(['pid', 'name', 'cmdline']):
        try:
            cmdline = proc.info['cmdline']
            if cmdline and 'compositor.py' in ' '.join(cmdline):
                compositor_procs.append(proc)
                print(f"Found compositor process: PID={proc.info['pid']}")
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    
    if not compositor_procs:
        print("ERROR: No compositor.py process found!")
        print("Make sure the compositor is running.")
        sys.exit(1)
    
    # Monitor main process
    proc = compositor_procs[0]
    print(f"\nMonitoring PID {proc.pid}...")
    print()
    
    # Print header
    print(f"{'Time':<12} {'CPU%':>8} {'MEM(MB)':>10} {'Threads':>8} {'FDs':>6} {'Status':<12}")
    print("-"*80)
    
    start_time = time.time()
    measurements = []
    
    try:
        while True:
            elapsed = time.time() - start_time
            
            if duration and elapsed > duration:
                break
            
            try:
                # Get process info
                cpu_percent = proc.cpu_percent(interval=0.1)
                mem_info = proc.memory_info()
                mem_mb = mem_info.rss / 1024 / 1024
                num_threads = proc.num_threads()
                num_fds = proc.num_fds() if hasattr(proc, 'num_fds') else 0
                status = proc.status()
                
                # Store measurement
                measurements.append({
                    'time': elapsed,
                    'cpu': cpu_percent,
                    'mem': mem_mb,
                    'threads': num_threads
                })
                
                # Print current stats
                time_str = f"{int(elapsed)}s"
                print(f"{time_str:<12} {cpu_percent:>7.1f}% {mem_mb:>9.0f}MB {num_threads:>8} {num_fds:>6} {status:<12}", 
                      flush=True)
                
                # Wait for next interval
                time.sleep(interval)
                
            except psutil.NoSuchProcess:
                print("\nProcess terminated!")
                break
    
    except KeyboardInterrupt:
        print("\n\nMonitoring stopped by user")
    
    # Print summary
    if measurements:
        print("\n" + "="*80)
        print("SUMMARY")
        print("="*80)
        
        cpu_values = [m['cpu'] for m in measurements]
        mem_values = [m['mem'] for m in measurements]
        
        print(f"Duration: {elapsed:.1f}s")
        print(f"Measurements: {len(measurements)}")
        print()
        print(f"CPU Usage:")
        print(f"  Average: {sum(cpu_values)/len(cpu_values):.1f}%")
        print(f"  Min: {min(cpu_values):.1f}%")
        print(f"  Max: {max(cpu_values):.1f}%")
        print()
        print(f"Memory Usage:")
        print(f"  Average: {sum(mem_values)/len(mem_values):.0f}MB")
        print(f"  Min: {min(mem_values):.0f}MB")
        print(f"  Max: {max(mem_values):.0f}MB")
        print()
        
        # Detect potential issues
        avg_cpu = sum(cpu_values)/len(cpu_values)
        if avg_cpu > 80:
            print("⚠  WARNING: High average CPU usage detected (>80%)")
            print("   Consider optimizing encoding settings:")
            print("   - Set X264_PRESET=ultrafast")
            print("   - Reduce X264_BITRATE (e.g., 1000-1500)")
            print("   - Increase WATCHDOG_INTERVAL_MS (e.g., 1000)")
        elif avg_cpu > 50:
            print("ℹ  INFO: Moderate CPU usage (50-80%)")
            print("   This is normal for video encoding")
        else:
            print("✓  CPU usage is within acceptable range")
        
        print("="*80)


def monitor_all_processes():
    """Monitor all processes to find CPU hogs."""
    print("Top CPU-consuming processes:\n")
    
    processes = []
    for proc in psutil.process_iter(['pid', 'name', 'cpu_percent', 'memory_info']):
        try:
            proc.cpu_percent(interval=0.1)  # Initial call
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    
    time.sleep(1)  # Wait to get accurate CPU measurement
    
    for proc in psutil.process_iter(['pid', 'name', 'cpu_percent', 'memory_info']):
        try:
            info = proc.info
            processes.append({
                'pid': info['pid'],
                'name': info['name'],
                'cpu': proc.cpu_percent(interval=0),
                'mem': info['memory_info'].rss / 1024 / 1024 if info['memory_info'] else 0
            })
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    
    # Sort by CPU usage
    processes.sort(key=lambda x: x['cpu'], reverse=True)
    
    print(f"{'PID':<8} {'Name':<30} {'CPU%':>8} {'MEM(MB)':>10}")
    print("-"*65)
    
    for p in processes[:20]:  # Top 20
        print(f"{p['pid']:<8} {p['name']:<30} {p['cpu']:>7.1f}% {p['mem']:>9.0f}MB")


def main():
    parser = argparse.ArgumentParser(
        description='Monitor CPU usage of compositor.py process',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Monitor with 2s interval indefinitely
  python3 monitor_cpu.py
  
  # Monitor for 60 seconds with 1s interval
  python3 monitor_cpu.py --interval 1 --duration 60
  
  # Show all processes
  python3 monitor_cpu.py --all
        """
    )
    
    parser.add_argument('-i', '--interval', type=float, default=2,
                        help='Measurement interval in seconds (default: 2)')
    parser.add_argument('-d', '--duration', type=float, default=None,
                        help='Total monitoring duration in seconds (default: infinite)')
    parser.add_argument('-a', '--all', action='store_true',
                        help='Show all processes instead of just compositor')
    
    args = parser.parse_args()
    
    if args.all:
        monitor_all_processes()
    else:
        monitor_compositor_cpu(interval=args.interval, duration=args.duration)


if __name__ == '__main__':
    main()