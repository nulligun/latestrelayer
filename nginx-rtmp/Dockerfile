# Multi-stage build for nginx with nginx-http-flv-module
# Based on: https://github.com/winshining/nginx-http-flv-module

# =============================================================================
# Stage 1: Build nginx with nginx-http-flv-module
# =============================================================================
FROM debian:bookworm-slim AS builder

# Version configuration
ARG NGINX_VERSION=1.26.2
ARG HTTP_FLV_MODULE_VERSION=master

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    curl \
    git \
    libpcre3-dev \
    libssl-dev \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Download and extract nginx source
WORKDIR /tmp
RUN curl -fSL https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz -o nginx.tar.gz \
    && tar -xzf nginx.tar.gz \
    && rm nginx.tar.gz

# Clone nginx-http-flv-module
RUN git clone --depth 1 https://github.com/winshining/nginx-http-flv-module.git

# Build nginx with nginx-http-flv-module
WORKDIR /tmp/nginx-${NGINX_VERSION}
RUN ./configure \
    --prefix=/usr/local/nginx \
    --sbin-path=/usr/local/nginx/sbin/nginx \
    --conf-path=/etc/nginx/nginx.conf \
    --error-log-path=/var/log/nginx/error.log \
    --http-log-path=/var/log/nginx/access.log \
    --pid-path=/var/run/nginx.pid \
    --lock-path=/var/run/nginx.lock \
    --with-http_ssl_module \
    --with-http_realip_module \
    --with-http_gzip_static_module \
    --with-http_secure_link_module \
    --with-http_stub_status_module \
    --with-ipv6 \
    --add-module=/tmp/nginx-http-flv-module \
    && make -j$(nproc) \
    && make install

# =============================================================================
# Stage 2: Runtime image
# =============================================================================
FROM debian:bookworm-slim

# Install runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gettext-base \
    libpcre3 \
    libssl3 \
    zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /tmp/hls /var/log/nginx

# Copy nginx from builder stage
COPY --from=builder /usr/local/nginx /usr/local/nginx
COPY --from=builder /etc/nginx /etc/nginx

# Create symlink for nginx binary in PATH
RUN ln -sf /usr/local/nginx/sbin/nginx /usr/local/bin/nginx

# Copy stat.xsl for RTMP statistics display
COPY --from=builder /tmp/nginx-http-flv-module/stat.xsl /usr/local/nginx/html/stat.xsl

# Copy health check script (build context is project root)
COPY docker/nginx-rtmp-health-check.sh /usr/local/bin/health-check.sh
RUN chmod +x /usr/local/bin/health-check.sh

# Expose RTMP and HTTP ports
EXPOSE 1935 8080

# Default command
CMD ["nginx", "-g", "daemon off;"]