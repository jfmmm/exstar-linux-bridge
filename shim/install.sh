#!/usr/bin/env bash
# Swap EXStar's CommunicateLayer.dll for the stand-in, or put the original back.
# Usage: install.sh [--restore]   (close EXStar first)
set -eu
SHIM="$(cd "$(dirname "$0")" && pwd)"
DIR="$HOME/Games/EXStar/prefix/drive_c/Shining3d/EXStar/syncservice"
ORIG_SHA=7554c87683214f1113ec56b28c20f8f560669c020692c76faefbeff8af1d7166

if [ ! -e "$DIR/CommunicateLayer.dll.orig" ]; then
    [ "$(sha256sum "$DIR/CommunicateLayer.dll" | cut -d' ' -f1)" = "$ORIG_SHA" ] || { echo "unexpected original DLL, not backing up" >&2; exit 1; }
    cp -p "$DIR/CommunicateLayer.dll" "$DIR/CommunicateLayer.dll.orig"
fi

if [ "${1:-}" = "--restore" ]; then
    cp -p "$DIR/CommunicateLayer.dll.orig" "$DIR/CommunicateLayer.dll"
    echo "original restored"
else
    cp "$SHIM/build/CommunicateLayer.dll" "$DIR/CommunicateLayer.dll"
    echo "stand-in installed"
fi
