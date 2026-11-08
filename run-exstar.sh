#!/usr/bin/env bash
# Launch EXStar through umu/Proton in its own prefix.
# Usage: run-exstar.sh [path\inside\app.exe]   (defaults to EXStar.exe)
BASE="$HOME/Games/EXStar"
APP_DIR="$BASE/prefix/drive_c/Shining3d/EXStar"

# A second launch would take the USB bridge away from the running copy.
if pgrep -f '[E]XStar\.exe \./' > /dev/null; then
    exit 0
fi

export WINEPREFIX="$BASE/prefix" GAMEID=umu-exstar STORE=none
# Private copy of Proton Experimental with nvidia-libs (real CUDA) installed.
export PROTONPATH="$BASE/proton"
export PROTON_LOG=1 PROTON_LOG_DIR="$BASE/logs"
export PROTON_ENABLE_NVAPI=1 DXVK_ENABLE_NVAPI=1
export __GLX_VENDOR_LIBRARY_NAME=nvidia __VK_LAYER_NV_optimus=NVIDIA_only
# UI scale for the 4K monitor; override with EXSTAR_SCALE=1.5 etc.
export QT_SCALE_FACTOR="${EXSTAR_SCALE:-2}"

cd "$APP_DIR" || exit 1

# USB bridge for the CommunicateLayer stand-in; the token keeps other local programs out.
BRIDGE="$BASE/shim/build/usb-bridge"
if [ -x "$BRIDGE" ]; then
    pkill -x usb-bridge && sleep 0.5
    TOKEN="$APP_DIR/syncservice/commshim.token"
    (umask 077; head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$TOKEN")
    "$BRIDGE" "$TOKEN" >> "$BASE/logs/usb-bridge.log" 2>&1 &
    BRIDGE_PID=$!
    trap 'kill "$BRIDGE_PID" 2>/dev/null' EXIT
fi

umu-run "$APP_DIR/${1:-EXStar.exe}"
