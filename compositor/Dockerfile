FROM debian:trixie-slim

# Install Python, GStreamer, and all dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 \
    python3-gi \
    python3-gst-1.0 \
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
COPY compositor.py /app/
COPY entrypoint.sh /app/

# Make scripts executable
RUN chmod +x /app/compositor.py /app/entrypoint.sh

# Set Python to run unbuffered
ENV PYTHONUNBUFFERED=1

# Expose ports
EXPOSE 1937/udp
EXPOSE 1940/tcp
EXPOSE 5000/tcp

# Run the application
ENTRYPOINT ["/app/entrypoint.sh"]