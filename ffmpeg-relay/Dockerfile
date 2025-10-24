FROM linuxserver/ffmpeg:latest

# Install additional tools
RUN apt-get update && apt-get install -y \
    curl \
    xmlstarlet \
    libzmq5 \
    && rm -rf /var/lib/apt/lists/*

# Create log directory
RUN mkdir -p /var/log/relayer

# Copy entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

WORKDIR /app

# Entrypoint
ENTRYPOINT ["/entrypoint.sh"]