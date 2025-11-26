# Stage 1: Build frontend
FROM node:20-alpine AS frontend-builder

WORKDIR /app/frontend

# tooling for native deps + git/ssh, and glibc compatibility shims
RUN apk add --no-cache \
    python3 make g++ pkgconfig \
    git openssh \
    libc6-compat ca-certificates

RUN npm i -g npm@10

# Copy frontend package files
COPY frontend/package*.json ./

RUN node -v && npm -v && \
    npm config set loglevel=verbose && \
    npm config set progress=false && \
    npm config set fetch-timeout 600000 && \
    env

# Install frontend dependencies with retry logic
RUN npm config set fetch-retries 5 && \
    npm config set fetch-retry-mintimeout 20000 && \
    npm config set fetch-retry-maxtimeout 120000 && \
    npm install --no-audit

# Copy frontend source
COPY frontend/ ./

# Move index.html to root where Vite expects it
RUN mv public/index.html ./index.html

# Build frontend for production
RUN npm run build

# Stage 2: Production
FROM node:20-alpine

WORKDIR /app

RUN apk add --no-cache \
    python3 make g++ pkgconfig \
    git openssh \
    curl \
    ffmpeg \
    libc6-compat ca-certificates
RUN npm i -g npm@10

# Copy backend package files
COPY backend/package*.json ./

# Install backend dependencies (production only) with retry logic
RUN npm config set fetch-retries 5 && \
    npm config set fetch-retry-mintimeout 20000 && \
    npm config set fetch-retry-maxtimeout 120000 && \
    npm install --omit=dev --no-audit

# Copy backend source
COPY backend/ ./

# Copy built frontend from previous stage
COPY --from=frontend-builder /app/frontend/dist ./frontend/dist

# Expose port
EXPOSE 3000

# Set environment variables with defaults
ENV PORT=3000
ENV CONTROLLER_API=http://controller:8089
ENV MUXER_API=http://muxer:8088
ENV NGINX_STATS=http://nginx-rtmp:8080/stat
ENV POLLING_INTERVAL=2000

# Health check
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD node -e "require('http').get('http://localhost:3000/api/health', (r) => { process.exit(r.statusCode === 200 ? 0 : 1); }).on('error', () => process.exit(1));"

# Start the application
CMD ["node", "server.js"]