#!/bin/bash
#
# Fallback Source Selector Script
# Reads fallback_config.json and returns the appropriate .ts file path
# for the ffmpeg-fallback container to stream.
#
# Output:
#   - Returns the path to the correct fallback .ts file
#   - BLACK  -> /media/fallback.ts (default BRB screen, auto-generated if missing)
#   - IMAGE  -> /media/static-image.ts (converted from uploaded image)
#   - VIDEO  -> /media/video.ts (converted from uploaded video)
#
# Note: The entire shared folder is mounted to /media in the container

CONFIG_FILE="/media/fallback_config.json"
DEFAULT_FALLBACK="/media/fallback.ts"

# If config file doesn't exist, use default fallback
if [ ! -f "$CONFIG_FILE" ]; then
    echo "[selector] Config file not found, using default fallback" >&2
    echo "$DEFAULT_FALLBACK"
    exit 0
fi

# Read the source value from the JSON config
# Using grep/sed since jq may not be available in minimal containers
SOURCE=$(grep -o '"source"[[:space:]]*:[[:space:]]*"[^"]*"' "$CONFIG_FILE" | sed 's/.*"source"[[:space:]]*:[[:space:]]*"\([^"]*\)"/\1/')

# If source is empty or couldn't be read, use default
if [ -z "$SOURCE" ]; then
    echo "[selector] Could not read source from config, using default fallback" >&2
    echo "$DEFAULT_FALLBACK"
    exit 0
fi

echo "[selector] Selected source: $SOURCE" >&2

case "$SOURCE" in
    "BLACK")
        FALLBACK_FILE="$DEFAULT_FALLBACK"
        ;;
    "IMAGE")
        FALLBACK_FILE="/media/static-image.ts"
        # Check if the file exists
        if [ ! -f "$FALLBACK_FILE" ]; then
            echo "[selector] WARNING: static-image.ts not found, falling back to default" >&2
            FALLBACK_FILE="$DEFAULT_FALLBACK"
        fi
        ;;
    "VIDEO")
        FALLBACK_FILE="/media/video.ts"
        # Check if the file exists
        if [ ! -f "$FALLBACK_FILE" ]; then
            echo "[selector] WARNING: video.ts not found, falling back to default" >&2
            FALLBACK_FILE="$DEFAULT_FALLBACK"
        fi
        ;;
    *)
        echo "[selector] Unknown source '$SOURCE', using default fallback" >&2
        FALLBACK_FILE="$DEFAULT_FALLBACK"
        ;;
esac

echo "[selector] Using fallback file: $FALLBACK_FILE" >&2

# Output the selected file path (this is what ffmpeg will use)
echo "$FALLBACK_FILE"