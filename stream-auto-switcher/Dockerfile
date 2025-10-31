FROM python:3.11-slim

# Prevent interactive prompts during build
ENV DEBIAN_FRONTEND=noninteractive

# Install curl for health checks
RUN apt-get update && \
    apt-get install -y --no-install-recommends curl && \
    rm -rf /var/lib/apt/lists/*

# Create app directory
WORKDIR /app

# Copy the Python script
COPY auto_switcher.py /app/auto_switcher.py
RUN chmod +x /app/auto_switcher.py

# Run with unbuffered output
CMD ["python3", "-u", "/app/auto_switcher.py"]