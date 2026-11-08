# EXStar Linux bridge

Runs Shining 3D's EXStar software with an EinStar scanner on Linux under Proton.

EXStar reaches the scanner through `CommunicateLayer.dll` and a KMDF driver
(`SH3DU3Driver.sys`), and Wine cannot load KMDF drivers. This project replaces
that path:

- `shim/commlayer.c` is a stand-in `CommunicateLayer.dll` with the same exports.
- `shim/usb-bridge.c` is a native Linux program that owns the scanner through
  libusb and does the driver's work, including its image cache.
- The two talk over a loopback socket protected by a random token.

No Shining 3D files are included. You need your own EXStar install.

## Status

Tested on Bazzite with an RTX 5080: connecting, camera streaming, calibration
and scanning.

## Setup

The scripts expect this layout:

```
~/Games/EXStar/
  prefix/      Wine prefix, with EXStar in drive_c/Shining3d/EXStar
  proton/      Proton copy with nvidia-libs, so EXStar gets real CUDA
  shim/        this repo's source
  run-exstar.sh
```

Allow your user to open the scanner, then replug it:

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="3267", MODE="0660", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/70-shining3d.rules
```

```bash
sudo udevadm control --reload
```

Build, which needs `gcc-mingw-w64-x86-64` and libusb development headers:

```bash
shim/build.sh
```

Install the stand-in with EXStar closed. The original DLL is kept as
`CommunicateLayer.dll.orig`, and `shim/install.sh --restore` puts it back.

```bash
shim/install.sh
```

Start EXStar with `run-exstar.sh`, which also starts the bridge.

## Safety limits

The scanner's bulk channel can reach its flash memory, so the bridge is
deliberately restrictive:

- Only command opcodes listed in `ALLOWED_OPS` are forwarded. Anything else is
  refused and logged to `logs/usb-bridge.log`.
- Flash page reads are allowed. **All flash writes are refused.**

EXStar reports a refused write as successful. Calibration results reach local
files but are not stored on the scanner, and EXStar loads calibration from the
scanner at launch, so calibration may need redoing after a restart.

Do not run the firmware updater through this bridge.

## Disclaimer

Unofficial and not affiliated with Shining 3D. Use at your own risk.
