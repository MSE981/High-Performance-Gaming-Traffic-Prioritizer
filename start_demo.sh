#!/usr/bin/env bash
# =============================================================================
# start_demo.sh — Build and run phase-5 demo executables (no Qt GUI, no eglfs)
#
# Usage:
#   ./start_demo.sh              # build if needed, run all demos in order
#   ./start_demo.sh nat_demo     # run a single demo by name
#
# First-time package install on Pi (if compilers missing):
#   sudo ./start_demo.sh
#
# Override build directory (same as release):
#   HPGTP_BUILD_DIR=build-user ./start_demo.sh
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="${HPGTP_BUILD_DIR:-build}"
BIN_DIR="$SCRIPT_DIR/$BUILD_DIR"

DEMOS_ORDER=(nat_demo dns_demo dhcp_demo scheduler_demo firewall_demo)

# ── 1. Dependencies (toolchain only — no Qt / ethtool) ───────────────────────
echo "[1/3] Checking build dependencies..."

REQUIRED_PKGS=(build-essential cmake gcc-14 g++-14)
pkg_installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed"; }

MISSING=()
for pkg in "${REQUIRED_PKGS[@]}"; do
    pkg_installed "$pkg" || MISSING+=("$pkg")
done

if [[ ${#MISSING[@]} -gt 0 ]]; then
    if [[ $EUID -ne 0 ]]; then
        echo "[Error] Missing packages: ${MISSING[*]}"
        echo "        Run:  sudo apt-get update && sudo apt-get install -y ${MISSING[*]}"
        echo "        Or run this script once as root:  sudo ./start_demo.sh"
        exit 1
    fi
    echo "    Installing: ${MISSING[*]}"
    apt-get update -qq
    apt-get install -y "${MISSING[@]}"
fi
echo "    Dependencies : OK"

# ── 2. Build ─────────────────────────────────────────────────────────────────
echo ""
echo "[2/3] Build check (demos + libs)..."

STAMP="$BIN_DIR/nat_demo"
NEEDS_BUILD=false
for d in "${DEMOS_ORDER[@]}"; do
    [[ -x "$BIN_DIR/$d" ]] || { NEEDS_BUILD=true; break; }
done

if ! $NEEDS_BUILD && [[ -f "$STAMP" ]]; then
    if [[ "$SCRIPT_DIR/CMakeLists.txt" -nt "$STAMP" ]]; then
        echo "    CMakeLists.txt newer — rebuilding."
        NEEDS_BUILD=true
    elif [[ -n "$(find "$SCRIPT_DIR/src" "$SCRIPT_DIR/include" "$SCRIPT_DIR/demo" \
                 \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" \) \
                 -newer "$STAMP" 2>/dev/null | head -1)" ]]; then
        echo "    Sources newer than demos — rebuilding."
        NEEDS_BUILD=true
    fi
fi

if $NEEDS_BUILD; then
    mkdir -p "$BIN_DIR"
    cmake -S "$SCRIPT_DIR" -B "$BIN_DIR" \
          -DCMAKE_BUILD_TYPE=Release \
          -Wno-dev \
          --log-level=WARNING
    cmake --build "$BIN_DIR" -j"$(nproc)"
    echo "    Build : OK"
else
    echo "    Demo binaries up to date."
fi

# ── 3. Run ───────────────────────────────────────────────────────────────────
echo ""
echo "[3/3] Running demos..."

run_demo() {
    local name="$1"
    local exe="$BIN_DIR/$name"
    if [[ ! -x "$exe" ]]; then
        echo "[Error] Not executable: $exe (target missing from build?)"
        return 1
    fi
    echo ""
    echo "══════════════════════════════════════════════════════════"
    echo "  $name"
    echo "══════════════════════════════════════════════════════════"
    "$exe"
}

if [[ $# -eq 0 ]] || [[ "${1:-}" == "all" ]]; then
    for d in "${DEMOS_ORDER[@]}"; do
        run_demo "$d"
    done
    echo ""
    echo "=== All listed demos finished ==="
else
    run_demo "$1"
fi
