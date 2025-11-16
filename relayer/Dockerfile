FROM debian:trixie-slim

# Install Python, GStreamer, and all dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 \
    python3-gi \
    python3-gst-1.0 \
    python3-psutil \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-tools \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy application files
COPY main.py /app/
COPY config.py /app/
COPY state_manager.py /app/
COPY privacy_manager.py /app/
COPY pipeline_builder.py /app/
COPY video_source_manager.py /app/
COPY srt_source_manager.py /app/
COPY http_api.py /app/
COPY relayer_manager.py /app/
COPY profiler.py /app/
COPY monitor_cpu.py /app/
COPY entrypoint.sh /app/
COPY healthcheck.sh /app/
COPY PROFILING_GUIDE.md /app/

# Make scripts executable
RUN chmod +x /app/main.py /app/monitor_cpu.py /app/entrypoint.sh /app/healthcheck.sh

# Set Python to run unbuffered
ENV PYTHONUNBUFFERED=1

# Expose ports
EXPOSE 1937/udp
EXPOSE 1940/tcp
EXPOSE 5000/tcp
EXPOSE 8088/tcp

# Run the application
ENTRYPOINT ["/app/entrypoint.sh"]