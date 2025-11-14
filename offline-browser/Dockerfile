FROM debian:bullseye-slim

# Install Xvfb, Chromium, FFmpeg, PulseAudio, and dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    xvfb \
    chromium \
    chromium-driver \
    ffmpeg \
    pulseaudio \
    x11-utils \
    procps \
    bash \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy scripts
COPY entrypoint.sh /app/
COPY healthcheck.sh /app/

# Make scripts executable
RUN chmod +x /app/entrypoint.sh /app/healthcheck.sh

# Set environment defaults
ENV RETRY_DELAY=1
ENV MAX_RETRY_DELAY=10
ENV OFFLINE_SOURCE_URL=https://example.com
ENV COMPOSITOR_HOST=compositor
ENV COMPOSITOR_PORT=1940
ENV DISPLAY_RESOLUTION=1920x1080
ENV FRAME_RATE=30
ENV DISPLAY=:99

# Run the entrypoint script
ENTRYPOINT ["/app/entrypoint.sh"]