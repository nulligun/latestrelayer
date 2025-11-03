FROM debian:bullseye-slim

# Install ffmpeg, curl, libxml2-utils (for xmllint), and procps for diagnostics and health checks
RUN apt-get update && \
    apt-get install -y ffmpeg curl libxml2-utils procps && \
    rm -rf /var/lib/apt/lists/*

# Create entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]