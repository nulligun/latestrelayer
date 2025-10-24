FROM alpine:3.19

# Install nginx with rtmp module and dependencies
RUN apk add --no-cache \
    nginx \
    nginx-mod-rtmp \
    curl \
    xmlstarlet \
    && rm -rf /var/cache/apk/*

# Create necessary directories
RUN mkdir -p /var/lib/nginx/tmp \
    && chown -R nginx:nginx /var/lib/nginx \
    && mkdir -p /var/log/nginx

# Copy nginx configuration
COPY nginx.conf /etc/nginx/nginx.conf

# Expose RTMP and HTTP stats ports
EXPOSE 1935 8080

# Start nginx in foreground
CMD ["nginx", "-g", "daemon off;"]