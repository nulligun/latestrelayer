FROM debian:bookworm-slim AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    libpcre3-dev \
    libssl-dev \
    zlib1g-dev \
    wget \
    git \
    && rm -rf /var/lib/apt/lists/*

# Download nginx and nginx-http-flv-module
WORKDIR /tmp
ARG NGINX_VERSION=1.24.0
RUN wget http://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz \
    && tar -xzf nginx-${NGINX_VERSION}.tar.gz \
    && git clone --branch master https://github.com/winshining/nginx-http-flv-module.git

# Build nginx with the module
WORKDIR /tmp/nginx-${NGINX_VERSION}
RUN ./configure \
    --prefix=/usr/local/nginx \
    --with-http_ssl_module \
    --with-http_v2_module \
    --with-http_realip_module \
    --with-http_stub_status_module \
    --with-threads \
    --with-file-aio \
    --add-module=/tmp/nginx-http-flv-module \
    && make -j$(nproc) \
    && make install

# Production image
FROM debian:bookworm-slim

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libpcre3 \
    libssl3 \
    zlib1g \
    curl \
    gettext-base \
    && rm -rf /var/lib/apt/lists/*

# Copy nginx from builder
COPY --from=builder /usr/local/nginx /usr/local/nginx

# Create necessary directories and symlinks for compatibility
RUN mkdir -p /tmp/hls /tmp/dash /var/log/nginx /etc/nginx /opt/nginx/html \
    && ln -sf /usr/local/nginx/conf/nginx.conf /etc/nginx/nginx.conf \
    && ln -sf /usr/local/nginx/html /opt/nginx/html

# Copy health check script (build context is project root)
COPY docker/nginx-rtmp-health-check.sh /usr/local/bin/health-check.sh
RUN chmod +x /usr/local/bin/health-check.sh

# Add nginx to PATH
ENV PATH="/usr/local/nginx/sbin:$PATH"

# Expose RTMP and HTTP ports
EXPOSE 1935 8080

# Start nginx in foreground
CMD ["nginx", "-g", "daemon off;"]