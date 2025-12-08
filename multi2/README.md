# TS Loop to RTMP

Seamlessly loop MPEG-TS files to RTMP or concatenate multiple TS files.

## Build

```bash
docker compose build
```

## Usage

### Stream to RTMP (Looping)

Stream a single TS file in an infinite loop to an RTMP server:

```bash
docker compose run --rm ts_loop_to_rtmp \
  --input /app/videos/fallback.ts \
  --rtmp-url rtmp://localhost/live/stream
```

**Options:**
- `--input FILE` - Input TS file (required)
- `--rtmp-url URL` - RTMP destination URL (required)
- `--quiet` - Only show errors

### Concatenate Files to Output

Splice multiple TS files together and output to a file:

```bash
docker compose run --rm ts_loop_to_rtmp \
  /app/videos/file1.ts /app/videos/file2.ts /app/videos/file3.ts > output.ts
```

Or stream the concatenated result to RTMP:

```bash
docker compose run --rm ts_loop_to_rtmp sh -c \
  'ts_loop_to_rtmp /app/videos/file1.ts /app/videos/file2.ts | \
   ffmpeg -f mpegts -i pipe:0 -c copy -f flv rtmp://localhost/live/stream'
```

### TCP Splicer - Stream from Live TCP Sources

Capture from one or two live TCP MPEG-TS streams. Works in two modes:

#### Single Source Mode

Continuously pull from a single TCP stream for the specified duration × loop count:

```bash
# Rebuild to get the latest changes
docker compose build

# Pull 30 seconds total (10s × 3 loops) from one TCP stream
docker compose run --rm ts_loop_to_rtmp \
  ts_tcp_splicer -duration 10 -loop 3 \
  tcp://127.0.0.1:9000 > output.ts
```

**Testing single source:**
```bash
# Terminal 1: Start TCP stream server (FFmpeg listens on port 9000)
./stream_tcp.sh -p smptebars -port 9000

# Terminal 2: Capture 30 seconds (10s × 3 loops)
docker compose run --rm ts_loop_to_rtmp \
  ts_tcp_splicer -duration 10 -loop 3 \
  tcp://127.0.0.1:9000 > output.ts

# Verify the output
ffprobe output.ts 2>&1 | grep Duration  # Should show ~30 seconds
ffplay output.ts
```

#### Dual Source Mode

Seamlessly alternate between two TCP streams:

```bash
# Alternate between two streams, 10s each, loop twice (total: 40s)
docker compose run --rm ts_loop_to_rtmp \
  ts_tcp_splicer -duration 10 -loop 2 \
  tcp://127.0.0.1:9000 tcp://127.0.0.1:9001 > output.ts
```

**How it works:**
- Pulls 10 seconds from TCP stream 1
- Switches to TCP stream 2 for 10 seconds
- Repeats the cycle for specified loops
- Maintains continuous timestamps with IDR-aligned splicing (clean switches)

**Testing dual source (requires 3 terminals):**
```bash
# Terminal 1: Start first TCP stream server with SMPTE bars
./stream_tcp.sh -p smptebars -port 9000

# Terminal 2: Start second TCP stream server with color bars
./stream_tcp.sh -p colorbars -port 9001

# Terminal 3: Run the splicer (10s each stream, loop twice = 40s total)
docker compose run --rm ts_loop_to_rtmp \
  ts_tcp_splicer -duration 10 -loop 2 \
  tcp://127.0.0.1:9000 tcp://127.0.0.1:9001 > output.ts

# Verify the output
ffprobe output.ts 2>&1 | grep Duration  # Should show ~40 seconds
ffplay output.ts  # Should show alternating SMPTE bars / color bars patterns
```

**Options:**
- `-duration SECONDS` - Duration to pull from each stream (required)
- `-loop N` - Number of times to loop (default: 1)
  - Single source: pulls N segments of specified duration
  - Dual source: alternates between sources N times

**Transport: TCP vs UDP**
- Uses TCP for reliable delivery (no packet loss)
- FFmpeg acts as TCP server (listen mode), application connects as client
- Simpler reassembly (continuous byte stream vs datagram boundaries)
- Automatic reconnection on disconnect for long-running services

**Requirements:**
- FFmpeg must be started with `tcp://host:port?listen=1` flag
- Stream(s) must have regular IDR frames (keyframes)
- For dual source mode, both streams must be compatible (same resolution, codec, etc.)
- Each stream will be analyzed for PAT/PMT/SPS/PPS automatically

**Note:** The splicer will connect to FFmpeg and wait for stream(s) to be ready (PAT/PMT discovered and IDR frame detected) before starting. This may take a few seconds initially.

## View Stream

```bash
# With VLC
vlc rtmp://localhost/live/stream

# With FFplay
ffplay rtmp://localhost/live/stream
```

## Generate Test Content

```bash
# Generate a 10-second test video
./generate_fallback.sh -d 10 -p smptebars

# Convert to MPEG-TS format
./convert_fallback.sh
```

This creates `videos/fallback.ts` optimized for seamless looping.

## Start Local RTMP Server (Optional)

```bash
docker run -d -p 1935:1935 --name rtmp-server tiangolo/nginx-rtmp
```

## License

MIT License