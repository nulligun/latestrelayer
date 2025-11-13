FROM debian:trixie-slim

# Install Python, GStreamer, and all dependencies together from Debian repos
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 \
    python3-gi \
    python3-gi-cairo \
    gir1.2-gstreamer-1.0 \
    gir1.2-gst-plugins-base-1.0 \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    libsrt1.5-openssl \
    ffmpeg \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy application files
COPY switcher.py /app/
COPY config.py /app/
COPY ffmpeg_process.py /app/
COPY gstreamer_simple.py /app/
COPY gstreamer_elements.py /app/
COPY srt_monitor.py /app/
COPY stream_switcher.py /app/
COPY api_server.py /app/
COPY entrypoint.sh /app/

# Make entrypoint executable
RUN chmod +x /app/entrypoint.sh

# Create directory for video files
RUN mkdir -p /videos

# Set Python to run unbuffered
ENV PYTHONUNBUFFERED=1

# Health check
HEALTHCHECK --interval=15s --timeout=5s --start-period=30s --retries=3 \
    CMD python3 -c "import urllib.request; urllib.request.urlopen('http://localhost:8088/health')" || exit 1

# Expose API port
EXPOSE 8088

# Run the application
ENTRYPOINT ["/app/entrypoint.sh"]