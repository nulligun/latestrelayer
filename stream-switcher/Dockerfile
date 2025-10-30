FROM ubuntu:22.04

# Prevent interactive prompts during build
ENV DEBIAN_FRONTEND=noninteractive

# Install GStreamer, Python, and dependencies
RUN apt-get update && \
    apt-get install -y \
    python3 \
    python3-pip \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-rtsp \
    python3-gi \
    python3-gst-1.0 \
    gir1.2-gstreamer-1.0 \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    && rm -rf /var/lib/apt/lists/*

# Create app directory
WORKDIR /app

# Copy the Python script
COPY rtmp_switcher.py /app/rtmp_switcher.py
RUN chmod +x /app/rtmp_switcher.py

# Expose API port
EXPOSE 8088

# Run the switcher
CMD ["python3", "/app/rtmp_switcher.py"]