FROM python:3.11-slim

# Prevent interactive prompts during build
ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies for Docker CLI
RUN apt-get update && \
    apt-get install -y \
        curl \
        ca-certificates \
        gnupg \
        lsb-release && \
    rm -rf /var/lib/apt/lists/*

# Add Docker's official GPG key and repository
RUN install -m 0755 -d /etc/apt/keyrings && \
    curl -fsSL https://download.docker.com/linux/debian/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg && \
    chmod a+r /etc/apt/keyrings/docker.gpg && \
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/debian $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | tee /etc/apt/sources.list.d/docker.list > /dev/null

# Install Docker CLI and Compose plugin
RUN apt-get update && \
    apt-get install -y \
        docker-ce-cli \
        docker-compose-plugin && \
    rm -rf /var/lib/apt/lists/*

# Install Docker Python SDK, PyYAML, and websockets
RUN pip install --no-cache-dir docker PyYAML websockets

# Create app directory
WORKDIR /app

# Copy the Python script (will be overridden by mount if present)
COPY container_controller.py /opt/controller.py
RUN chmod +x /opt/controller.py

# Expose HTTP API port and WebSocket port
EXPOSE 8089 8090

# Run the controller with unbuffered output (-u flag)
# The PROJECT_PATH env var will tell it where the mounted project is
CMD ["python3", "-u", "/opt/controller.py"]