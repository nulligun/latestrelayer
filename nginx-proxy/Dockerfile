FROM nginx:alpine

# Install required packages for SSL, htpasswd, and health checks
RUN apk add --no-cache \
    openssl \
    apache2-utils \
    curl

# Copy nginx configuration
COPY nginx.conf /etc/nginx/nginx.conf

# Copy entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# Copy health check script
COPY health-check.sh /usr/local/bin/health-check.sh
RUN chmod +x /usr/local/bin/health-check.sh

# Create directories for SSL certificates and auth
RUN mkdir -p /etc/nginx/ssl /etc/nginx/auth

# Expose HTTPS port
EXPOSE 443

# Use custom entrypoint
ENTRYPOINT ["/entrypoint.sh"]