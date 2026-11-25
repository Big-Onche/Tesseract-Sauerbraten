#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BINARY="$SCRIPT_DIR/binaries/mac/sauerract"

if [ ! -d "$SCRIPT_DIR/packages/base" ]; then
    echo "Sauerract requires an existing Sauerbraten installation." >&2
    echo "Place this overlay in the Sauerbraten installation root (packages/base is missing)." >&2
    exit 1
fi

if [ ! -x "$BINARY" ]; then
    echo "Sauerract binary not found or not executable: $BINARY" >&2
    echo "Build it from src_sauerract or install a release binary." >&2
    exit 1
fi

cd "$SCRIPT_DIR" || exit 1
exec "$BINARY" "-u$HOME/Library/Application Support/Sauerract" "$@"
