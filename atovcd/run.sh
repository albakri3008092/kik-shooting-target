#!/usr/bin/env bash
# Start the ATOVCD console on the local Wi-Fi (tablet opens http://<pi-ip>:8000/).
set -euo pipefail
cd "$(dirname "$0")"
exec python3 -m uvicorn app.main:app --host "${ATOVCD_HOST:-0.0.0.0}" --port "${ATOVCD_PORT:-8000}"
