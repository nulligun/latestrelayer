FROM alpine:3.19

# Install FFmpeg
RUN apk add --no-cache \
    ffmpeg \
    bash

# Copy entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]