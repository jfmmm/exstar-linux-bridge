#!/usr/bin/env bash
# Build the stand-in DLL and the Linux USB bridge. Run inside the claude distrobox.
set -eu
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-gcc -shared -O2 -Wall -Wextra -s -static-libgcc -o build/CommunicateLayer.dll commlayer.c commlayer.def -lws2_32
gcc -O2 -Wall -Wextra -s -pthread -o build/usb-bridge usb-bridge.c -lusb-1.0
