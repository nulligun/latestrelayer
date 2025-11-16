# Compositor CPU Profiling - Quick Reference

## 🚀 Quick Start (30 seconds)

```bash
# 1. Rebuild & restart
docker-compose build compositor && docker-compose up -d compositor

# 2. Monitor CPU usage
docker exec -it compositor python3 /app/monitor_cpu.py --duration 30

# 3. Check optimization logs
docker-compose logs compositor | grep cpu-opt
```

## 📊 Environment Variables

```bash
# Recommended defaults (already set in v2.4.0)
X264_PRESET=ultrafast           # Lowest CPU usage
X264_BITRATE=1500               # Good quality/CPU balance
WATCHDOG_INTERVAL_MS=500        # Adequate responsiveness
ENABLE_CPU_PROFILING=false      # Enable for detailed analysis
```

## 🔧 Common Scenarios

### Scenario 1: Still too much CPU
```bash
X264_PRESET=ultrafast
X264_BITRATE=1000
WATCHDOG_INTERVAL_MS=1000
```

### Scenario 2: Need better quality
```bash
X264_PRESET=superfast
X264_BITRATE=2500
WATCHDOG_INTERVAL_MS=500
```

### Scenario 3: Debugging performance
```bash
ENABLE_CPU_PROFILING=true
GST_DEBUG=x264enc:5
```

## 📈 Expected Performance

| State | Old CPU | New CPU | Savings |
|-------|---------|---------|---------|
| Idle (black screen) | ~85% | ~45% | **-47%** |
| Active streaming | ~115% | ~70% | **-39%** |

## 🛠️ Monitoring Commands

```bash
# Quick check (30s)
docker exec -it compositor python3 /app/monitor_cpu.py --duration 30

# Continuous monitoring
docker exec -it compositor python3 /app/monitor_cpu.py

# See all processes
docker exec -it compositor python3 /app/monitor_cpu.py --all

# Check settings
docker exec -it compositor env | grep X264
```

## 🎯 Preset Quick Reference

| Preset | CPU | Quality | Use When |
|--------|-----|---------|----------|
| ultrafast | ⚡⚡⚡ | ★★★☆☆ | CPU-constrained (default) |
| superfast | ⚡⚡☆ | ★★★★☆ | Balanced |
| veryfast | ⚡☆☆ | ★★★★★ | CPU available |

## 📖 Documentation Files

- **[CPU_OPTIMIZATION_USAGE.md](CPU_OPTIMIZATION_USAGE.md)** - Complete usage guide
- **[PROFILING_GUIDE.md](PROFILING_GUIDE.md)** - Detailed profiling documentation
- **[monitor_cpu.py](monitor_cpu.py)** - Standalone monitoring tool
- **[profiler.py](profiler.py)** - Profiling utilities library

## 🆘 Troubleshooting

### Verify optimizations applied
```bash
docker-compose logs compositor | grep "cpu-opt"
```
Should show:
```
[cpu-opt] x264 CPU optimization: preset=ultrafast
[cpu-opt] SRT watchdog interval: 500ms
```

### Check actual CPU
```bash
docker stats compositor
```

### Get detailed report
```bash
docker exec -it compositor python3 /app/monitor_cpu.py --duration 60 > report.txt
```

---
**Version:** 2.4.0 | **Updated:** 2025-01-16