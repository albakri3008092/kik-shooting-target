#!/usr/bin/env bash
# Copies dashboard/src -> firmware/central_receiver/data
# Run this before using "ESP32 Sketch Data Upload" in Arduino IDE.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="$ROOT/dashboard/src"
DST="$ROOT/firmware/central_receiver/data"
rm -rf "$DST"
mkdir -p "$DST"
cp -R "$SRC"/. "$DST"/
echo "Copied dashboard to $DST"
