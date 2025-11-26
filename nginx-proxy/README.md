# Nginx Proxy

HTTPS reverse proxy with basic authentication for the Stream Dashboard.

## Features

- **HTTPS on port 443** with auto-generated self-signed SSL certificate
- **HTTP Basic Authentication** for access control
- **WebSocket support** for real-time dashboard updates
- **Cloudflare-ready** with proper proxy headers
- **Large file upload support** (up to 500MB)

## Configuration

Set these environment variables in your [`../.env`](../.env:1) file:

```bash
NGINX_USER=admin
NGINX_PASSWORD=changeme
```

## Usage

### Start the proxy

```bash
docker-compose up -d nginx-proxy
```

The proxy will:
1. Generate a self-signed SSL certificate on first startup (persisted in `./shared/ssl/`)
2. Create an htpasswd file with your credentials
3. Start serving HTTPS on port 443

**Note:** SSL certificates are persisted on your host filesystem, so you won't need to accept new certificates after container restarts.

### Access the dashboard

Navigate to `https://localhost:443` (or your domain if using Cloudflare)

You'll be prompted for:
- Username: (from `NGINX_USER`)
- Password: (from `NGINX_PASSWORD`)

### With Cloudflare

Configure Cloudflare SSL/TLS settings:
- **SSL/TLS encryption mode**: Full (not Full strict)
- The self-signed certificate is sufficient for Cloudflare's "Full" mode
- Cloudflare will handle the trusted SSL certificate for your visitors

## Files

- [`Dockerfile`](Dockerfile:1) - Container build configuration
- [`nginx.conf`](nginx.conf:1) - Nginx configuration with proxy rules
- [`entrypoint.sh`](entrypoint.sh:1) - Startup script for SSL cert and auth setup

## Security Notes

- Self-signed certificates are acceptable when behind Cloudflare
- Basic auth credentials are transmitted securely over HTTPS
- The dashboard port (3000) is no longer exposed directly to the host
- All access must go through the authenticated HTTPS proxy

## Troubleshooting

### Check logs
```bash
docker-compose logs nginx-proxy
```

### Regenerate SSL certificate
Delete the cached SSL certificate on the host:
```bash
rm -rf ./shared/ssl/*
docker-compose restart nginx-proxy
```

The new certificate will be generated and persisted to `./shared/ssl/`.

### Test WebSocket connection
The proxy is configured with proper WebSocket upgrade headers. Check browser console for any WebSocket connection errors.