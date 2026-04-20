#!/usr/bin/env bash
# =============================================================================
# start_release.sh — high-performance gaming traffic prioritizer (release / production GUI)
# One-shot setup and launch for Raspberry Pi 5 + DSI 800x1280 portrait display
#
# Usage:  chmod +x start_release.sh && sudo ./start_release.sh
#         Optional: user-owned build tree (e.g. after root-owned build/):
#           sudo env HPGTP_BUILD_DIR=build-user ./start_release.sh
#         Skip kernel prep (ip_forward, iptables MASQUERADE on WAN, NM/dnsmasq):
#           sudo env HPGTP_SKIP_KERNEL_NET_PREP=1 ./start_release.sh
# =============================================================================
set -euo pipefail

# -- Sanity -------------------------------------------------------------------
if [[ $EUID -ne 0 ]]; then
    echo "[Error] Must be run as root:  sudo ./start_release.sh"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"   # binary loads config/config.txt relative to CWD

BUILD_DIR="${HPGTP_BUILD_DIR:-build}"

NOTIFY_LOG="/tmp/hpgtp_startup.log"
> "$NOTIFY_LOG"
notify() { echo "${1}|${2}" >> "$NOTIFY_LOG"; }

echo "=========================================================="
echo "  High-performance gaming traffic prioritizer v3.0 (release)"
echo "=========================================================="
echo "    Root: $SCRIPT_DIR"
echo ""

# -- 1. Dependencies ----------------------------------------------------------
echo "[1/4] Checking dependencies..."

REQUIRED_PKGS=(
    build-essential
    cmake
    gcc-14
    g++-14
    qt6-base-dev
    qt6-base-dev-tools
    ethtool
)

OPTIONAL_PKGS=(
    qt6-qpa-plugins
    speedtest-cli
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
notify "[1/4] Dependencies" "eglfs: $(basename "$EGLFS_SO") | All packages OK"

# -- 2. Build (release main binary; demo/ changes do not force rebuild) -------
echo ""
echo "[2/4] Build check..."

BINARY="$SCRIPT_DIR/$BUILD_DIR/GamingTrafficPrioritizer"

NEEDS_BUILD=false
if [[ ! -x "$BINARY" ]]; then
    NEEDS_BUILD=true
elif [[ "$SCRIPT_DIR/CMakeLists.txt" -nt "$BINARY" ]]; then
    echo "    CMakeLists.txt newer than binary -- rebuilding."
    NEEDS_BUILD=true
elif [[ -n "$(find "$SCRIPT_DIR/src" "$SCRIPT_DIR/include" \
             \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" \) \
             -newer "$BINARY" 2>/dev/null | head -1)" ]]; then
    echo "    Source modified -- rebuilding."
    NEEDS_BUILD=true
fi

if $NEEDS_BUILD; then
    mkdir -p "$SCRIPT_DIR/$BUILD_DIR"
    cmake -S "$SCRIPT_DIR" -B "$SCRIPT_DIR/$BUILD_DIR" \
          -DCMAKE_BUILD_TYPE=Release \
          -Wno-dev \
          --log-level=WARNING
    cmake --build "$SCRIPT_DIR/$BUILD_DIR" -j"$(nproc)"
    echo "    Build : OK  ($BINARY)"
    notify "[2/4] Build" "Rebuilt OK | $(basename "$BINARY")"
else
    echo "    Binary up to date."
    notify "[2/4] Build" "Binary up to date | $(basename "$BINARY")"
fi

# -- 3. Network interfaces -----------------------------------------------------
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

NET_STATUS=""
for iface in "$WAN_IFACE" "$LAN_IFACE"; do
    if ip link show "$iface" &>/dev/null; then
        ethtool -K "$iface" gro off gso off tso off rx off tx off 2>/dev/null && \
            echo "    $iface : offloads disabled" || \
            echo "    $iface : ethtool partial (non-fatal)"
        ip link set "$iface" up 2>/dev/null || true
        NET_STATUS="${NET_STATUS}${iface}:OK  "
    else
        echo "    [Warn] $iface not found -- verify IFACE_WAN/IFACE_LAN in config/config.txt"
        NET_STATUS="${NET_STATUS}${iface}:MISSING  "
    fi
done
notify "[3/4] Network" "WAN=$WAN_IFACE LAN=$LAN_IFACE | ${NET_STATUS% }"

# -- 3b. Kernel hygiene for userspace NAT/forwarding -----------------------------
# ip_forward off; drop only POSTROUTING MASQUERADE -o WAN (does not remove ts-postrouting jumps).
echo ""
if [[ "${HPGTP_SKIP_KERNEL_NET_PREP:-}" == "1" ]]; then
    echo "    [3b] Skipped (HPGTP_SKIP_KERNEL_NET_PREP=1)"
    notify "[3b] Kernel prep" "skipped"
else
    echo "    [3b] ip_forward=0, MASQUERADE -o $WAN_IFACE removed, dnsmasq stop, NM LAN unmanaged"
    sysctl -w net.ipv4.ip_forward=0 >/dev/null
    n=0
    command -v iptables >/dev/null 2>&1 && {
        while iptables -t nat -C POSTROUTING -o "$WAN_IFACE" -j MASQUERADE 2>/dev/null; do
            iptables -t nat -D POSTROUTING -o "$WAN_IFACE" -j MASQUERADE
            ((++n))
        done
    }
    echo "        removed $n MASQUERADE rule(s); dnsmasq+nmcli best-effort"
    command -v systemctl >/dev/null 2>&1 && systemctl stop dnsmasq 2>/dev/null || true
    command -v nmcli >/dev/null 2>&1 && nmcli device set "$LAN_IFACE" managed no 2>/dev/null || true
    notify "[3b] Kernel prep" "ip_forward=0 masq_rm=$n"
fi

# -- 4. Launch ----------------------------------------------------------------
echo ""
echo "[4/4] Launching GUI (eglfs, 800x1280 portrait)..."
echo ""
notify "[4/4] Launch" "eglfs 800x1280 | DSI-2 portrait"

export QT_QPA_PLATFORM=eglfs
export QT_QPA_EGLFS_HIDECURSOR=1
export QT_QPA_EGLFS_KMS_CONFIG="$SCRIPT_DIR/config/kms.json"
export QT_LOGGING_RULES="qt.qpa.*=false"

exec "$BINARY"
