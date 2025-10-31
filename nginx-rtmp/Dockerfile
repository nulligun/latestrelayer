FROM debian:bullseye-slim

# Install nginx with RTMP module and diagnostic tools
RUN apt-get update && \
    apt-get install -y \
    nginx \
    libnginx-mod-rtmp \
    net-tools \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy configuration files
COPY nginx.conf /etc/nginx/nginx.conf
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 1936 8080

ENTRYPOINT ["/entrypoint.sh"]