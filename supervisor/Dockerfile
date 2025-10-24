FROM python:3.11-slim

# Install system dependencies
RUN apt-get update && apt-get install -y \
    curl \
    xmlstarlet \
    && rm -rf /var/lib/apt/lists/*

# Create app directory
WORKDIR /app

# Copy requirements and install Python dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy supervisor script
COPY supervisor.py .

# Create log directory
RUN mkdir -p /var/log/relayer

# Run supervisor
CMD ["python", "-u", "supervisor.py"]