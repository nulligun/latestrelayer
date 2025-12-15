#!/bin/sh
set -e

PIPE_DIR="/pipe"
echo "=== Named Pipe Initialization ==="

mkdir -p "$PIPE_DIR"

# Remove stale pipes from previous runs
echo "[pipe-init] Removing stale pipes..."
rm -f "$PIPE_DIR/camera.ts" "$PIPE_DIR/fallback.ts" "$PIPE_DIR/ts_output.pipe"

# Create named pipes
echo "[pipe-init] Creating named pipes..."

mkfifo "$PIPE_DIR/camera.ts"
echo "[pipe-init] Created /pipe/camera.ts"

mkfifo "$PIPE_DIR/fallback.ts"
echo "[pipe-init] Created /pipe/fallback.ts"

mkfifo "$PIPE_DIR/ts_output.pipe"
echo "[pipe-init] Created /pipe/ts_output.pipe"

# Set permissions
chmod 666 "$PIPE_DIR/camera.ts"
chmod 666 "$PIPE_DIR/fallback.ts"
chmod 666 "$PIPE_DIR/ts_output.pipe"

echo "[pipe-init] Pipes created successfully:"
ls -la "$PIPE_DIR"

echo "[pipe-init] Pipe initialization complete"
