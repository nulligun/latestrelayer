FROM linuxserver/ffmpeg:latest

# Install bash and procps for health checks
RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    procps \
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
ENV COMPOSITOR_HOST=compositor
ENV COMPOSITOR_PORT=5000
ENV CONFIG_PATH=/app/shared/kick_config.json

# Run the entrypoint script
ENTRYPOINT ["/app/entrypoint.sh"]