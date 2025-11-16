FROM nginx:alpine

# Install required packages for SSL and htpasswd
RUN apk add --no-cache \
    openssl \
    apache2-utils

# Copy nginx configuration
COPY nginx.conf /etc/nginx/nginx.conf

# Copy entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# Create directories for SSL certificates and auth
RUN mkdir -p /etc/nginx/ssl /etc/nginx/auth

# Expose HTTPS port
EXPOSE 443

# Use custom entrypoint
ENTRYPOINT ["/entrypoint.sh"]