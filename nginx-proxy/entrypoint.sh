#!/bin/sh
set -e

echo "==================================="
echo "Nginx Proxy - Starting Up"
echo "==================================="

# SSL Certificate generation
SSL_CERT="/etc/nginx/ssl/cert.pem"
SSL_KEY="/etc/nginx/ssl/key.pem"

if [ ! -f "$SSL_CERT" ] || [ ! -f "$SSL_KEY" ]; then
    echo "[ssl] Generating self-signed SSL certificate..."
    openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
        -keyout "$SSL_KEY" \
        -out "$SSL_CERT" \
        -subj "/C=US/ST=State/L=City/O=Organization/CN=localhost" \
        2>/dev/null
    echo "[ssl] SSL certificate generated successfully"
else
    echo "[ssl] Using existing SSL certificate"
fi

# Basic auth setup
AUTH_FILE="/etc/nginx/auth/.htpasswd"

if [ -n "$NGINX_USER" ] && [ -n "$NGINX_PASSWORD" ]; then
    echo "[auth] Setting up basic authentication..."
    htpasswd -bc "$AUTH_FILE" "$NGINX_USER" "$NGINX_PASSWORD"
    echo "[auth] Basic authentication configured for user: $NGINX_USER"
else
    echo "[auth] WARNING: NGINX_USER and NGINX_PASSWORD not set - authentication disabled!"
    # Create empty auth file to prevent nginx errors
    touch "$AUTH_FILE"
fi

echo "[nginx] Starting Nginx..."
echo "==================================="

# Start nginx in foreground
exec nginx -g 'daemon off;'