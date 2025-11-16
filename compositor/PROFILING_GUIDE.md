# Compositor CPU Profiling Guide

This guide explains how to profile and optimize CPU usage in the compositor container.

## Quick Start

### 1. Check Current CPU Usage

```bash
# Inside compositor container
docker exec -it compositor python3 /app/monitor_cpu.py --duration 30
```

### 2. Enable Built-in Profiling

```bash
# In docker-compose.yml or .env
ENABLE_CPU_PROFILING=true
```

The compositor will print detailed timing reports every 10 seconds.

### 3. Optimize Settings

```bash
# Set environment variables for lower CPU usage
X264_PRESET=ultrafast        # Lowest CPU (default)
X264_BITRATE=1500            # Lower bitrate = less CPU
WATCHDOG_INTERVAL_MS=500     # Less frequent checks
```

## Environment Variables

### CPU Optimization Settings

| Variable | Default | Description | Impact |
|----------|---------|-------------|--------|
| `X264_PRESET` | `ultrafast` | x264 encoding preset | 🔥 High - `ultrafast` uses ~30% less CPU than `superfast` |
| `X264_BITRATE` | `1500` | Video bitrate in kbps | 🔥 High - Lower bitrate = less encoding work |
| `WATCHDOG_INTERVAL_MS` | `500` | Watchdog timer interval | ⚡ Medium - Reduces polling frequency |
| `ENABLE_CPU_PROFILING` | `false` | Enable detailed profiling | 📊 Adds ~1-2% overhead |

### x264 Preset Comparison

| Preset | CPU Usage | Quality | Recommended For |
|--------|-----------|---------|-----------------|
| `ultrafast` | ~40-60% | Good | Low-latency, CPU-constrained systems |
| `superfast` | ~70-90% | Better | Balanced performance |
| `veryfast` | ~100%+ | Best | High-quality, powerful systems |

### GStreamer Debug

Enable GStreamer debug logging:

```bash
GST_DEBUG=x264enc:5         # Debug x264 encoder
GST_DEBUG=compositor:5      # Debug compositor element
GST_DEBUG=*:3               # Medium verbosity on all
GST_DEBUG=*:5               # High verbosity (very noisy!)
```

## Monitoring Tools

### 1. Standalone Monitor Script

```bash
# Basic monitoring (2s interval, infinite)
docker exec -it compositor python3 /app/monitor_cpu.py

# Monitor for 60 seconds with 1s interval
docker exec -it compositor python3 /app/monitor_cpu.py --interval 1 --duration 60

# Show all processes in container
docker exec -it compositor python3 /app/monitor_cpu.py --all
```

**Output Example:**
```
Time         CPU%   MEM(MB)  Threads    FDs Status      
--------------------------------------------------------------------------------
0s           45.2%      156MB        8     45 sleeping    
2s           47.8%      158MB        8     45 sleeping    
4s           46.1%      159MB        8     45 sleeping    

SUMMARY
================================================================================
Duration: 30.0s
Measurements: 15

CPU Usage:
  Average: 46.4%
  Min: 42.1%
  Max: 51.3%
```

### 2. Built-in Profiling

Enable with `ENABLE_CPU_PROFILING=true` and restart compositor.

**Output Example:**
```
[cpu-monitor] ========================================
[cpu-monitor] Process CPU: 48.2% | Memory: 162MB | Threads: 8
[cpu-monitor] x264enc: {'name': 'x264enc', 'state': 'GST_STATE_PLAYING'}
[cpu-monitor] compositor: {'name': 'compositor', 'state': 'GST_STATE_PLAYING'}
[cpu-monitor] audiomixer: {'name': 'audiomixer', 'state': 'GST_STATE_PLAYING'}

======================================================================
PROFILING REPORT
======================================================================
Function                           Calls    Avg(ms)    Max(ms)  Total(ms)
----------------------------------------------------------------------
watchdog_cb                          123       0.15       2.34      18.45
video_watchdog_cb                    123       0.12       1.89      14.76
_on_srt_video_probe                 1847       0.03       0.98      55.41
_on_video_probe                        0       0.00       0.00       0.00
======================================================================
```

### 3. System-level Monitoring

```bash
# Top command inside container
docker exec -it compositor top

# Watch CPU usage
docker stats compositor

# Get detailed process info
docker exec -it compositor ps aux
```

## Common CPU Issues

### Issue: 100%+ CPU when idle (no streaming)

**Symptoms:**
- CPU usage stays high even with no SRT/video connections
- Encoding black screen at full speed

**Solutions:**
1. ✅ Set `X264_PRESET=ultrafast` (default in v2.4.0+)
2. ✅ Set `X264_BITRATE=1000` or lower for static content
3. ✅ Set `WATCHDOG_INTERVAL_MS=1000` to reduce polling

**Configuration:**
```yaml
environment:
  X264_PRESET: ultrafast
  X264_BITRATE: 1000
  WATCHDOG_INTERVAL_MS: 1000
```

### Issue: CPU spikes during streaming

**Symptoms:**
- CPU jumps to 100%+ when SRT connects
- Stuttering or frame drops

**Solutions:**
1. Reduce resolution at source (e.g., 720p instead of 1080p)
2. Lower bitrate: `X264_BITRATE=1200`
3. Check network stability (packet loss causes re-encoding)

### Issue: Watchdog callbacks consuming CPU

**Symptoms:**
- Profiling shows `watchdog_cb` taking significant time
- Many watchdog checks per second

**Solutions:**
1. ✅ Increase interval: `WATCHDOG_INTERVAL_MS=1000` (default: 500ms)
2. Check for network issues causing constant reconnections

## Performance Benchmarks

### Idle (Black Screen Only)

| Config | CPU Usage | Notes |
|--------|-----------|-------|
| `superfast` @ 2500kbps | ~85% | Old default |
| `ultrafast` @ 1500kbps | ~45% | New default ✅ |
| `ultrafast` @ 1000kbps | ~35% | Lowest |

### Active Streaming (1080p30 SRT)

| Config | CPU Usage | Quality |
|--------|-----------|---------|
| `ultrafast` @ 1500kbps | ~60-70% | Good |
| `superfast` @ 2500kbps | ~100%+ | Better |
| `veryfast` @ 3000kbps | ~150%+ | Best |

## GStreamer Pipeline Profiling

### View Pipeline Graph

```bash
# Set debug output location
export GST_DEBUG_DUMP_DOT_DIR=/tmp

# Enable dot file generation (add to compositor.py temporarily)
os.environ['GST_DEBUG_DUMP_DOT_DIR'] = '/tmp'

# Generate graph in PLAYING state
# After compositor starts, find the .dot files in /tmp

# Convert to image (requires graphviz)
dot -Tpng /tmp/pipeline.dot > pipeline.png
```

### CPU Profiling with gst-top

```bash
# Install gst-top (if available)
apt-get install gst-top

# Run gst-top
gst-top --pipeline compositor
```

## Troubleshooting

### Profiler not working

**Error:** `profiler.py not found`

**Solution:**
```bash
# Ensure profiler.py is in compositor directory
ls -la /app/profiler.py

# Rebuild container if missing
docker-compose build compositor
docker-compose up -d compositor
```

### psutil ImportError

**Error:** `ImportError: No module named 'psutil'`

**Solution:**
```bash
# Install psutil in container
docker exec -it compositor pip3 install psutil

# Or add to Dockerfile
RUN pip3 install psutil
```

### High CPU even with optimizations

Run comprehensive check:

```bash
# 1. Verify settings are applied
docker exec -it compositor env | grep X264
docker exec -it compositor env | grep WATCHDOG

# 2. Check actual x264enc settings in logs
docker logs compositor | grep "x264enc config"

# 3. Monitor for 1 minute
docker exec -it compositor python3 /app/monitor_cpu.py --duration 60

# 4. Check Docker resource limits
docker inspect compositor | grep -A 5 Resources
```

## Advanced: Python Profiling

For detailed Python-level profiling:

```python
import cProfile
import pstats

# In compositor.py main()
profiler = cProfile.Profile()
profiler.enable()

compositor.run()

profiler.disable()
stats = pstats.Stats(profiler)
stats.sort_stats('cumulative')
stats.print_stats(20)  # Top 20 functions
```

## Performance Optimization Checklist

- [ ] Set `X264_PRESET=ultrafast`
- [ ] Set `X264_BITRATE=1500` or lower
- [ ] Set `WATCHDOG_INTERVAL_MS=500` or higher
- [ ] Verify with `monitor_cpu.py` for 30-60 seconds
- [ ] Check logs for "cpu-opt" messages confirming settings
- [ ] Test stream quality is acceptable
- [ ] Document baseline CPU usage for comparison

## Getting Help

If CPU usage remains high after optimizations:

1. Run monitoring with `--duration 60` and save output
2. Check compositor logs: `docker logs compositor > compositor.log`
3. Include system info: CPU model, RAM, Docker version
4. Share profiling report if `ENABLE_CPU_PROFILING=true`

---

**Last Updated:** 2025-01-16  
**Compositor Version:** 2.4.0+