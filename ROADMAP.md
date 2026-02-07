# Project Roadmap

## Project Status

⚠️ **This project is currently on hold** unless someone can convince https://kick.com/xplorer to come out of retirement and do IRL streaming again. If that happens, development will resume with the priorities outlined below.

---

## Priorities (If Development Resumes)

### 1. Replace Custom Splicing with MediaMTX Always-Online
- Remove the custom MPEGTS splicing pipeline entirely
- Adopt [MediaMTX's always-online feature](https://github.com/bluenviron/mediamtx/pull/5335) for professional-grade stream splicing
- Leverage MediaMTX to handle source failover and fallback natively, eliminating the need for our C++ multiplexer and MPEGTS tooling
- Remove MPEGTS-related build targets, Dockerfiles, and scripts that are no longer needed

### 2. Frame Grab API Endpoint
- Add a new API endpoint to return frame grabs from recent I-frames
- Maintain a rolling buffer of approximately 30 seconds of I-frames
- This allows frame-grab timestamps to match what viewers are actually seeing on screen (accounting for stream delay)

### 3. Platform Support Expansion

#### Twitch Integration
- Add support for Twitch RTMP ingest
- Handle Twitch-specific stream requirements

#### YouTube Integration
- Add support for YouTube Live streaming
- Handle YouTube-specific stream requirements

### 4. Developer Documentation
- Create comprehensive documentation for connecting additional streaming services
- Provide examples and templates for service integration
- Document the API and extension points

---

## Long Term Goals

### Professional-Grade Stream Splicing via MediaMTX
- Leverage MediaMTX's [always-online feature](https://github.com/bluenviron/mediamtx/pull/5335) for broadcast-quality source switching
- Seamless stream switching with no visual artifacts, handled natively by MediaMTX
- Think: ad injection quality that forces ad-blockers into the compressed domain (TiVo-style)
- Goal is splicing so clean that transitions are undetectable to viewers and adblockers alike 😈

---

## Contributing

If you're interested in helping with any of these features, please open an issue to discuss.