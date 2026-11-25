#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
OUTPUT="$SCRIPT_DIR/sauerract-overlay.dmg"
STAGING=$(mktemp -d "${TMPDIR:-/tmp}/sauerract-overlay.XXXXXX") || exit 1
OVERLAY="$STAGING/Sauerract Overlay"

cleanup()
{
    rm -rf "$STAGING"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$OVERLAY"
ditto "$ROOT/binaries" "$OVERLAY/binaries"
ditto "$ROOT/sauerract" "$OVERLAY/sauerract"
ditto "$ROOT/src_sauerract" "$OVERLAY/src_sauerract"
ditto "$ROOT/sauerract.bat" "$OVERLAY/sauerract.bat"
ditto "$ROOT/sauerract.sh" "$OVERLAY/sauerract.sh"
ditto "$ROOT/sauerract.command" "$OVERLAY/sauerract.command"
ditto "$ROOT/README.md" "$OVERLAY/README.md"
ditto "$ROOT/LICENSE" "$OVERLAY/LICENSE"

rm -f "$OUTPUT"
hdiutil create -fs HFS+ -srcfolder "$STAGING" -volname "Sauerract Overlay" "$OUTPUT"
