FROM debian:bullseye-slim

# Install ffmpeg and netcat for connection checking
RUN apt-get update && \
    apt-get install -y ffmpeg netcat && \
    rm -rf /var/lib/apt/lists/*

# Create entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]