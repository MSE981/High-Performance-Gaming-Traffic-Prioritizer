#!/usr/bin/env bash
# =============================================================================
# start.sh — Scalpel Gaming Traffic Prioritizer
# One-shot setup and launch for Raspberry Pi 5 + DSI 800×1280 portrait display
#
# Usage:  sudo ./start.sh
# =============================================================================
set -euo pipefail

# ── Sanity ────────────────────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    echo "[Error] Must be run as root:  sudo ./start.sh"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"   # binary loads config/config.txt relative to CWD

echo "╔══════════════════════════════════════════════════════╗"
echo "║   Scalpel Gaming Traffic Prioritizer — v3.0          ║"
echo "╚══════════════════════════════════════════════════════╝"
echo "    Root: $SCRIPT_DIR"
echo ""

# ── 1. Dependencies ───────────────────────────────────────────────────────────
echo "[1/4] Checking dependencies..."

# Required packages
REQUIRED_PKGS=(
    build-essential
    cmake
    gcc-14
    g++-14
    qt6-base-dev
    qt6-base-dev-tools
    ethtool
)

# Optional packages (install silently if available; skip if not in apt index)
OPTIONAL_PKGS=(
    qt6-qpa-plugins      # eglfs platform plugin (may already be in qt6-base-dev on Pi OS)
    speedtest-cli        # ISP bandwidth probe
)

pkg_installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed"; }

MISSING_REQUIRED=()
for pkg in "${REQUIRED_PKGS[@]}"; do
    pkg_installed "$pkg" || MISSING_REQUIRED+=("$pkg")
done

if [[ ${#MISSING_REQUIRED[@]} -gt 0 ]]; then
    echo "    Installing required: ${MISSING_REQUIRED[*]}"
    apt-get update -qq
    apt-get install -y "${MISSING_REQUIRED[@]}"
fi

for pkg in "${OPTIONAL_PKGS[@]}"; do
    if ! pkg_installed "$pkg"; then
        apt-get install -y "$pkg" 2>/dev/null && echo "    Installed optional: $pkg" || \
            echo "    Skipped optional:  $pkg (not in apt index)"
    fi
done

# Verify the eglfs platform plugin is reachable
EGLFS_SO=$(find /usr/lib -name "libqeglfs.so" 2>/dev/null | head -1)
if [[ -z "$EGLFS_SO" ]]; then
    echo ""
    echo "[Error] Qt6 eglfs platform plugin not found."
    echo "        On Pi OS the plugin is usually inside qt6-base-dev."
    echo "        Try:  sudo apt install qt6-qpa-plugins  (or rebuild Qt6 with eglfs support)"
    exit 1
fi
echo "    eglfs plugin : $EGLFS_SO"
echo "    Dependencies : OK"

# ── 2. Build ──────────────────────────────────────────────────────────────────
echo ""
echo "[2/4] Build check..."

BINARY="$SCRIPT_DIR/build/GamingTrafficPrioritizer"

# Trigger rebuild if binary is absent or any source/header is newer than it
NEEDS_BUILD=false
if [[ ! -x "$BINARY" ]]; then
    NEEDS_BUILD=true
elif [[ -n "$(find "$SCRIPT_DIR/src" "$SCRIPT_DIR/include" -name "*.cpp" -o -name "*.hpp" \
             -newer "$BINARY" 2>/dev/null | head -1)" ]]; then
    echo "    Source modified — rebuilding."
    NEEDS_BUILD=true
fi

if $NEEDS_BUILD; then
    mkdir -p "$SCRIPT_DIR/build"
    cmake -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/build" \
          -DCMAKE_BUILD_TYPE=Release \
          -Wno-dev \
          --log-level=WARNING
    cmake --build "$SCRIPT_DIR/build" -j"$(nproc)"
    echo "    Build : OK  ($BINARY)"
else
    echo "    Binary up to date."
fi

# ── 3. Network interfaces ─────────────────────────────────────────────────────
echo ""
echo "[3/4] Network interface setup..."

CONFIG_FILE="$SCRIPT_DIR/config/config.txt"
WAN_IFACE="eth0"
LAN_IFACE="eth1"

if [[ -f "$CONFIG_FILE" ]]; then
    v=$(grep -m1 '^IFACE_WAN=' "$CONFIG_FILE" 2>/dev/null | cut -d= -f2 | tr -d '[:space:]')
    [[ -n "$v" ]] && WAN_IFACE="$v"
    v=$(grep -m1 '^IFACE_LAN=' "$CONFIG_FILE" 2>/dev/null | cut -d= -f2 | tr -d '[:space:]')
    [[ -n "$v" ]] && LAN_IFACE="$v"
fi

echo "    WAN=$WAN_IFACE  LAN=$LAN_IFACE"

for iface in "$WAN_IFACE" "$LAN_IFACE"; do
    if ip link show "$iface" &>/dev/null; then
        # Disable hardware offloads that corrupt raw-socket packet lengths (TPACKET path)
        ethtool -K "$iface" gro off gso off tso off rx off tx off 2>/dev/null && \
            echo "    $iface : offloads disabled" || \
            echo "    $iface : ethtool partial (non-fatal)"
        ip link set "$iface" up 2>/dev/null || true
    else
        echo "    [Warn] $iface not found — verify IFACE_WAN/IFACE_LAN in config/config.txt"
    fi
done

# ── 4. Launch ─────────────────────────────────────────────────────────────────
echo ""
echo "[4/4] Launching GUI (eglfs, 800×1280 portrait)..."
echo ""

# Qt eglfs: drive the DRM framebuffer directly — no Wayland compositor required.
# Native portrait resolution 800×1280 is used as-is; no rotation applied.
export QT_QPA_PLATFORM=eglfs                              # direct DRM framebuffer, no compositor
export QT_QPA_EGLFS_HIDECURSOR=1                          # DSI2 has no DRM cursor plane
export QT_QPA_EGLFS_KMS_CONFIG="$SCRIPT_DIR/config/kms.json"  # hwcursor:false — disables QEglFSKmsGbmCursor entirely
export QT_LOGGING_RULES="qt.qpa.*=false"                  # suppress platform plugin verbose output

exec "$BINARY"
