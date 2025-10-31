FROM python:3.11-slim

# Prevent interactive prompts during build
ENV DEBIAN_FRONTEND=noninteractive

# Install curl for healthchecks and Docker SDK
RUN apt-get update && \
    apt-get install -y curl && \
    rm -rf /var/lib/apt/lists/*

# Install Docker Python SDK
RUN pip install --no-cache-dir docker

# Create app directory
WORKDIR /app

# Copy the Python script
COPY container_controller.py /app/container_controller.py
RUN chmod +x /app/container_controller.py

# Expose API port
EXPOSE 8089

# Run the controller with unbuffered output (-u flag)
CMD ["python3", "-u", "/app/container_controller.py"]