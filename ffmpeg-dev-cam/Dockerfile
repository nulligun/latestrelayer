FROM debian:bullseye-slim

# Install ffmpeg and procps for health checks
RUN apt-get update && \
    apt-get install -y ffmpeg procps && \
    rm -rf /var/lib/apt/lists/*

# Create entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]