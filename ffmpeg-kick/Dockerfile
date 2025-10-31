FROM debian:bullseye-slim

# Install ffmpeg, curl, and libxml2-utils (for xmllint) for diagnostics
RUN apt-get update && \
    apt-get install -y ffmpeg curl libxml2-utils && \
    rm -rf /var/lib/apt/lists/*

# Create entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]