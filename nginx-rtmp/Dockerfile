FROM tiangolo/nginx-rtmp:latest

# Install curl for health checks and gettext-base for envsubst (nginx.conf templating)
RUN apt-get update && \
    apt-get install -y curl gettext-base && \
    rm -rf /var/lib/apt/lists/*

# Copy health check script (build context is project root)
COPY docker/nginx-rtmp-health-check.sh /usr/local/bin/health-check.sh
RUN chmod +x /usr/local/bin/health-check.sh

# Expose RTMP and HTTP ports
EXPOSE 1935 8080