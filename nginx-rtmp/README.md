# nginx-http-flv-module Setup

This directory contains a production-ready nginx build with [nginx-http-flv-module](https://github.com/winshining/nginx-http-flv-module) compiled from source, enabling advanced features like GOP cache and HTTP-FLV streaming.

## Why nginx-http-flv-module?

The standard `nginx-rtmp-module` does **NOT** support `gop_cache`. Only `nginx-http-flv-module` provides this critical feature for reducing initial stream latency.

### Key Features

- **GOP Cache**: Reduces initial playback latency by 2-4 seconds
- **HTTP-FLV**: Low-latency streaming protocol (alternative to RTMP)
- **HLS Support**: Standard HLS streaming
- **JSON Statistics**: Modern API for monitoring
- **All nginx-rtmp-module features**: Full backward compatibility

## Build Process

### Dependencies Installed

**Build-time:**
- `build-essential` - GCC compiler and build tools
- `libpcre3-dev` - PCRE library for regex support
- `libssl-dev` - OpenSSL for HTTPS/RTMPS
- `zlib1g-dev` - Compression support
- `wget` - Download nginx source
- `git` - Clone nginx-http-flv-module

**Runtime:**
- `libpcre3` - PCRE runtime
- `libssl3` - OpenSSL runtime
- `zlib1g` - Compression runtime
- `curl` - Health checks
- `gettext-base` - Config templating (envsubst)

### Configure Flags

```bash
./configure \
    --prefix=/usr/local/nginx \
    --with-http_ssl_module \          # HTTPS support
    --with-http_v2_module \           # HTTP/2 support
    --with-http_realip_module \       # Real IP detection behind proxies
    --with-http_stub_status_module \  # Basic status page
    --with-threads \                  # Thread pool support
    --with-file-aio \                 # Async file I/O
    --add-module=/tmp/nginx-http-flv-module
```

### Build Commands

```bash
# Build the image
docker build -t nginx-http-flv:latest -f nginx-rtmp/Dockerfile .

# Or using docker-compose
docker-compose build nginx-rtmp
```

## GOP Cache Configuration

### What is GOP Cache?

GOP (Group of Pictures) cache stores the last keyframe and subsequent frames, allowing new viewers to start playback immediately without waiting for the next keyframe.

### Configuration Options

```nginx
rtmp {
    server {
        application live {
            live on;
            gop_cache on;           # Enable GOP caching
            gop_cache_count 1;      # Number of GOPs to cache
        }
    }
}
```

### Optimal Settings by Use Case

| Use Case | `gop_cache_count` | Latency Reduction | Memory Impact | Best For |
|----------|-------------------|-------------------|---------------|----------|
| **Ultra Low Latency** | `1` | ~2-4 seconds | Low | Gaming, sports, auctions |
| **Balanced** | `2` | ~4-8 seconds | Medium | General live streaming |
| **Smooth Playback** | `3-4` | ~6-12 seconds | High | Concerts, presentations |

**Recommendation for most live streaming:** `gop_cache_count 1`

### How It Works

1. **Without GOP cache**: New viewer waits for next keyframe (2-10 seconds depending on GOP size)
2. **With GOP cache**: New viewer receives cached keyframe immediately + subsequent frames

**Example Timeline:**
```
Keyframe interval: 4 seconds
Without cache: 0-4 second wait (average 2 seconds)
With cache: Instant playback
```

## Streaming Protocols

### 1. RTMP (Publishing & Playback)

**Publish:**
```bash
ffmpeg -re -i input.mp4 -c copy -f flv rtmp://localhost:1935/live/stream1
```

**Play:**
```bash
ffplay rtmp://localhost:1935/live/stream1
```

### 2. HTTP-FLV (Low Latency Playback)

**URL Format:**
```
http://localhost:8080/live?app=live&stream=stream1
```

**Play with ffplay:**
```bash
ffplay "http://localhost:8080/live?app=live&stream=stream1"
```

**Play with flv.js (web):**
```javascript
const player = flvjs.createPlayer({
    type: 'flv',
    url: 'http://localhost:8080/live?app=live&stream=stream1',
    isLive: true
});
player.attachMediaElement(videoElement);
player.load();
player.play();
```

### 3. HLS (Standard Playback)

**URL:**
```
http://localhost:8080/hls/stream1.m3u8
```

**Play:**
```bash
ffplay http://localhost:8080/hls/stream1.m3u8
```

## Monitoring & Statistics

### JSON Statistics API

```bash
curl http://localhost:8080/stat
```

**Response includes:**
- Active streams
- Viewer counts
- Bandwidth usage
- Connection details
- GOP cache statistics

### Control API

```bash
# Drop a publisher
curl -X POST "http://localhost:8080/control/drop/publisher?app=live&name=stream1"

# Drop a subscriber
curl -X POST "http://localhost:8080/control/drop/subscriber?app=live&name=stream1"
```

## Performance Tuning

### For High Concurrent Viewers

```nginx
events {
    worker_connections 4096;  # Increase for more connections
}

rtmp {
    out_queue 4096;          # Output buffer size
    out_cork 8;              # Cork output packets
    max_streams 128;         # Max concurrent streams per worker
}
```

### For Low Latency

```nginx
rtmp {
    server {
        chunk_size 4096;     # Smaller chunks = lower latency
        
        application live {
            gop_cache on;
            gop_cache_count 1;  # Minimal cache
        }
    }
}
```

### For Stability

```nginx
rtmp {
    timeout 15s;                    # Connection timeout
    drop_idle_publisher 15s;        # Drop inactive publishers
}
```

## Troubleshooting

### GOP Cache Not Working

1. **Verify module is loaded:**
   ```bash
   nginx -V 2>&1 | grep nginx-http-flv-module
   ```

2. **Check configuration:**
   ```bash
   nginx -t
   ```

3. **Monitor logs:**
   ```bash
   docker logs -f nginx-rtmp
   ```

### High Memory Usage

- Reduce `gop_cache_count` from 2 to 1
- Decrease `worker_connections`
- Limit `max_streams`

### Playback Delays

- Ensure `gop_cache on` is set
- Check encoder GOP size (should be 2-4 seconds)
- Verify network latency
- Use HTTP-FLV instead of HLS for lower latency

## Comparison: nginx-rtmp-module vs nginx-http-flv-module

| Feature | nginx-rtmp-module | nginx-http-flv-module |
|---------|-------------------|----------------------|
| RTMP Streaming | ✅ | ✅ |
| HLS Streaming | ✅ | ✅ |
| HTTP-FLV | ❌ | ✅ |
| GOP Cache | ❌ | ✅ |
| JSON Stats | ❌ | ✅ |
| Virtual Hosts | Limited | ✅ |
| Audio-only | Limited | ✅ |

## References

- [nginx-http-flv-module GitHub](https://github.com/winshining/nginx-http-flv-module)
- [nginx-rtmp-module GitHub](https://github.com/arut/nginx-rtmp-module)
- [NGINX Documentation](http://nginx.org/en/docs/)