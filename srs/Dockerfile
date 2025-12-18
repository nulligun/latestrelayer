FROM ossrs/srs:5

# Install curl for health checks
USER root
RUN apt-get update && \
    apt-get install -y curl && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Switch back to default user if SRS uses one
USER root

# Use the default SRS entrypoint and command