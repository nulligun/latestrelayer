FROM alpine:3.19

# Install FFmpeg and utilities
RUN apk add --no-cache \
    ffmpeg \
    bash \
    curl

# Copy entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]