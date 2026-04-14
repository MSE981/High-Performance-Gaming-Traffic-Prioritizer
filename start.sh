#!/usr/bin/env bash
# =============================================================================
# start.sh — compatibility entrypoint (forwards to release)
#
# Prefer explicitly:
#   sudo ./start_release.sh   — production GUI on Pi + DSI
#   ./start_demo.sh           — build/run C++ demos (no Qt main app)
# =============================================================================
set -euo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/start_release.sh" "$@"
