# Multi-stage Dockerfile for ts_loop_to_rtmp with TSDuck
# Stage 1: Build application with pre-built TSDuck
FROM ubuntu:24.04 AS app-builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies and wget
RUN apt-get update && apt-get install -y \
    g++ \
    cmake \
    make \
    pkg-config \
    wget \
    libcurl4-openssl-dev \
    libssl-dev \
    libedit-dev \
    ffmpeg \
    zlib1g-dev \
    libusb-1.0-0-dev \
    && rm -rf /var/lib/apt/lists/*

# Download and install TSDuck pre-built packages (base + dev)
RUN wget https://github.com/tsduck/tsduck/releases/download/v3.42-4421/tsduck_3.42-4421.ubuntu24_amd64.deb \
    && wget https://github.com/tsduck/tsduck/releases/download/v3.42-4421/tsduck-dev_3.42-4421.ubuntu24_amd64.deb \
    && apt-get update \
    && apt-get install -y ./tsduck_3.42-4421.ubuntu24_amd64.deb ./tsduck-dev_3.42-4421.ubuntu24_amd64.deb \
    && rm tsduck_3.42-4421.ubuntu24_amd64.deb tsduck-dev_3.42-4421.ubuntu24_amd64.deb \
    && rm -rf /var/lib/apt/lists/*

# Create application directory
WORKDIR /app

# Copy CMakeLists.txt first (for better caching)
COPY CMakeLists.txt .

# Copy source code
COPY src/ ./src/

# Build the application
RUN mkdir -p build && cd build && \
    cmake .. && \
    make -j$(nproc)

# Stage 2: Runtime image
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Install only runtime dependencies
RUN apt-get update && apt-get install -y \
    libcurl4 \
    libssl3t64 \
    libedit2 \
    ffmpeg \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Download and install TSDuck runtime package
RUN wget https://github.com/tsduck/tsduck/releases/download/v3.42-4421/tsduck_3.42-4421.ubuntu24_amd64.deb \
    && apt-get update \
    && apt-get install -y ./tsduck_3.42-4421.ubuntu24_amd64.deb \
    && rm tsduck_3.42-4421.ubuntu24_amd64.deb \
    && rm -rf /var/lib/apt/lists/*

# Copy the built applications
COPY --from=app-builder /app/build/ts_loop_to_rtmp /usr/local/bin/
COPY --from=app-builder /app/build/ts_tcp_splicer /usr/local/bin/

# Set library path
ENV LD_LIBRARY_PATH=/usr/lib:${LD_LIBRARY_PATH}

# Create directory for videos
RUN mkdir -p /app/videos

WORKDIR /app

# Default command (can be overridden)
CMD ["ts_loop_to_rtmp"]