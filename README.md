# EXStar Linux bridge

Run Shining 3D's Windows-only **EXStar** software with an **EinStar** 3D scanner
on Linux, under Proton, with live scanning.

EXStar works under Proton once it has real CUDA, but it cannot see the scanner:
it reaches USB through a kernel driver (`SH3DU3Driver.sys`, KMDF) and Wine cannot
load KMDF drivers. This project replaces that path.

```
EXStar (under Proton)
  └─ CommunicateLayer.dll   ← stand-in from this repo, same 29 exports as the original
       └─ TCP on 127.0.0.1, authenticated with a random token
            └─ usb-bridge   ← native Linux program from this repo
                 └─ libusb → scanner
```

No Shining 3D files are included. You need your own copy of EXStar and your own
scanner.

**Contents**

1. [Status](#status)
2. [Requirements](#requirements)
3. [Installation](#installation)
4. [Using EXStar](#using-exstar)
5. [Safety limits](#safety-limits)
6. [Troubleshooting](#troubleshooting)
7. [Logs](#logs)
8. [Uninstalling](#uninstalling)
9. [Technical reference](#technical-reference)

## Status

| Feature | State |
|---|---|
| Install and launch under Proton | Working |
| GPU compute (CUDA) | Working, needs nvidia-libs |
| Sign-in and license check | Working |
| Connecting to the scanner | Working |
| Camera image streaming | Working |
| Calibration | Working, but **not stored on the scanner** (see [Safety limits](#safety-limits)) |
| Scanning | Working |
| Firmware updates | **Do not use** |
| Calibration guide videos | Do not play (missing codec); not required |

Tested with:

| Component | Version |
|---|---|
| OS | Bazzite (Fedora 44 base), `bazzite-nvidia-open`, KDE on Wayland |
| GPU | NVIDIA RTX 5080, driver 615.78.08 |
| EXStar | v1.2.2.0 |
| Installer SHA-256 | `3720a6a341fada78b34ca76e27ca9b03c90feecf092bcb64cb876cc9b54d1a37` |
| Proton | Proton Experimental 11.0 (`experimental-11.0-20261027b`) |
| umu-launcher | 1.4.4 |
| nvidia-libs | v1.0.5 |
| Scanner | EinStar, USB ID `3267:0003`, reports `EinScan10_01` |

Other versions may work. `shim/install.sh` deliberately refuses any
`CommunicateLayer.dll` other than the one from v1.2.2.0, and a different EXStar
version may also use commands the bridge does not allow.

## Requirements

- An NVIDIA GPU with the proprietary or open NVIDIA driver and CUDA
- Steam, with **Proton Experimental** installed (Library → Tools)
- [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher) (`umu-run`), which Bazzite includes
- `libusb-1.0` on the host
- A way to build the code: MinGW-w64, GCC and libusb headers. On an immutable
  distro such as Bazzite, use a distrobox container (step 7)
- About 4 GB of disk for the app and Proton copy, plus space for scan projects
- A USB connection to the scanner. It enumerated at 480 Mbps in testing

## Installation

Every step below assumes this variable. Set it again if you open a new terminal.

```bash
export B="$HOME/Games/EXStar"
```

All paths are fixed to `~/Games/EXStar`. The finished folder looks like this:

```
~/Games/EXStar/
├── README.md, run-exstar.sh, shim/   this repo
├── prefix/                           Wine prefix; EXStar in drive_c/Shining3d/EXStar
├── proton/                           private Proton copy with nvidia-libs
├── nvidia-libs-dl/                   nvidia-libs download
└── logs/                             Proton and bridge logs
```

`.gitignore` only admits this repo's files, so the rest stays out of git.

### 1. Get this repo

```bash
git clone https://github.com/jfmmm/exstar-linux-bridge.git "$B"
```

```bash
mkdir -p "$B/prefix" "$B/logs"
```

### 2. Download EXStar

Get the EXStar installer from Shining 3D, for example
`EXStar_v1.2.2.0.exe` (about 729 MB). Check that it matches the tested version:

```bash
sha256sum ~/Downloads/EXStar_v1.2.2.0.exe
```

### 3. Install EXStar with Proton

Make sure Proton Experimental is installed in Steam, then run the installer in
its own prefix. This never touches Steam's own prefixes.

```bash
WINEPREFIX="$B/prefix" GAMEID=umu-exstar STORE=none \
PROTONPATH="$HOME/.local/share/Steam/steamapps/common/Proton - Experimental" \
umu-run ~/Downloads/EXStar_v1.2.2.0.exe
```

In the installer, use the install location **`C:\Shining3d\EXStar`**, because the
launcher expects that path. The Windows USB driver cannot be used under Wine, and
this project replaces it, so ignore it if it does not install.

### 4. Make a private Proton copy with real CUDA

Proton's own `nvcuda.dll` is a stub, and without real CUDA EXStar closes partway
through its splash screen. [nvidia-libs](https://github.com/SveSop/nvidia-libs)
provides a working one. Install it into a private copy so Steam's Proton stays
untouched.

```bash
cp -a --reflink=auto "$HOME/.local/share/Steam/steamapps/common/Proton - Experimental" "$B/proton"
```

```bash
mkdir -p "$B/nvidia-libs-dl" && curl -fsSL https://github.com/SveSop/nvidia-libs/releases/download/v1.0.5/nvidia-libs-v1.0.5.tar.xz | tar xJ -C "$B/nvidia-libs-dl"
```

The setup script asks whether to install NVML. The piped `y` answers yes.

```bash
cd "$HOME" && echo y | PROTON_LIBS="$B/proton" bash "$B/nvidia-libs-dl/nvidia-libs-v1.0.5/proton_setup.sh"
```

Proton disables `nvcuda` in the prefix by default. Remove that override, and
delete the version marker so the prefix re-links to the private Proton:

```bash
sed -i '/^"nvcuda"="disabled"$/d' "$B/prefix/user.reg"
```

```bash
rm -f "$B/prefix/version"
```

### 5. Check that CUDA works (optional but recommended)

```bash
WINEPREFIX="$B/prefix" GAMEID=umu-exstar STORE=none PROTONPATH="$B/proton" PROTON_ENABLE_NVAPI=1 DXVK_ENABLE_NVAPI=1 umu-run "$B/prefix/drive_c/windows/system32/cmd.exe" /c "Z:$(echo "$B" | tr / '\\')\\nvidia-libs-dl\\nvidia-libs-v1.0.5\\bin\\cudatest.exe > C:\\cudatest.txt 2>&1"
```

```bash
tr -d '\000\r' < "$B/prefix/drive_c/cudatest.txt" | tail -20
```

You should see `cuInit success!` and your GPU.

### 6. Allow your user to open the scanner

Linux only lets root write to the scanner's USB device by default. Plug in the
scanner and run:

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="3267", MODE="0660", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/70-shining3d.rules
```

```bash
sudo udevadm control --reload
```

```bash
sudo udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=3267
```

To confirm, this should print `user:<you>:rw-`, using the device node for your
scanner (`lsusb` shows its bus and device numbers):

```bash
getfacl -p /dev/bus/usb/001/018
```

### 7. Build the DLL and bridge

On a regular distro, install MinGW-w64, GCC and libusb development headers with
your package manager and run `shim/build.sh`.

On Bazzite or another immutable distro, build in a Debian 12 distrobox. The
resulting bridge runs on the host.

```bash
distrobox create --name exstar-build --image debian:12 --yes
```

```bash
distrobox enter exstar-build -- sudo apt-get install -y --no-install-recommends build-essential gcc-mingw-w64-x86-64 libusb-1.0-0-dev
```

```bash
distrobox enter exstar-build -- "$B/shim/build.sh"
```

This creates `shim/build/CommunicateLayer.dll` and `shim/build/usb-bridge`.

### 8. Install the stand-in DLL

**Close EXStar first.** The first run backs up the original as
`CommunicateLayer.dll.orig`, next to it in `syncservice/`.

```bash
"$B/shim/install.sh"
```

To put the original back at any time:

```bash
"$B/shim/install.sh" --restore
```

### 9. Launch

```bash
"$B/run-exstar.sh"
```

The launcher generates a new token, starts the USB bridge, runs EXStar and stops
the bridge afterwards. If EXStar is already running it exits without doing
anything.

The UI scale defaults to 2×. To change it:

```bash
EXSTAR_SCALE=1.5 "$B/run-exstar.sh"
```

### 10. Add a menu shortcut (optional)

This extracts nothing and uses a generic icon. Replace `Icon=` with an image of
your choice if you like.

```bash
printf '[Desktop Entry]\nType=Application\nName=EXStar\nComment=EinStar 3D scanner software\nExec=%s/run-exstar.sh\nIcon=applications-graphics\nTerminal=false\nCategories=Graphics;3DGraphics;\n' "$B" > ~/.local/share/applications/exstar.desktop
```

## Using EXStar

**First launch.** Sign in with your Shining 3D account when asked. EXStar reads
the scanner's stored data at startup and goes on from there.

**Firmware.** Decline any firmware update. The updater has not been tested
through this bridge, and the bridge refuses flash writes anyway.

**Choosing where projects are saved.** EXStar's folder picker may only show
`C:`. A symlink inside `C:` makes a normal Linux folder available:

```bash
mkdir -p ~/Documents/EXStarProjects && ln -s ~/Documents/EXStarProjects "$B/prefix/drive_c/EXStarProjects"
```

Then pick `C:\EXStarProjects` in EXStar.

**Saving to a NAS.** Not tested. Mount the share on Linux somewhere under your
home folder or `/mnt`, then add a symlink in `C:` the same way. Saving locally
and copying afterwards is safer, because a large project written over the network
through Wine could be slow or fail partway.

**Calibrating.** Follow the on-screen targets, since the guide videos do not
play. Make sure the calibration plate faces the correct way, avoid direct light
that causes glare on the plate, and hold still at each position. EXStar rejects
results above an internal accuracy limit. Good runs here measured about 0.08
(left-right) and 0.10 (texture camera), and a run at 0.105 for left-right failed.

## Safety limits

The bulk channel can reach the scanner's flash memory, which holds calibration
and activation data. The bridge is deliberately restrictive:

- Only command opcodes listed in `ALLOWED_OPS` in `shim/usb-bridge.c` are
  forwarded. Anything else is refused and logged as `BLOCKED`.
- Flash **page reads** are allowed, matched exactly by `is_flash_page_read`.
- **All flash writes are refused.** They are logged as `BLOCKED bulk write`.
- The bridge only listens on `127.0.0.1` and requires the launcher's random token.

**Refused writes look successful.** EXStar's write path reports success even when
every attempt was refused, so a message saying calibration was saved does not mean
it reached the scanner. The result does go to local files under
`res/Einscan-E10/200x150/`, and the current session uses it. EXStar loads
calibration from the scanner at launch, so calibration will probably revert to
what the scanner has stored after a restart.

EXStar also tries to update activation data in flash on launch. That write is
refused too, and scanning has worked without it.

If you enable flash writes, back up flash pages 0 and 1 first. A bad write could
damage the scanner's stored calibration or activation data.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| EXStar closes partway through the splash screen | No real CUDA. The log shows `init_speckle gpu test exception` | Redo step 4 and check with step 5 |
| Scanner not detected, `enumDevice deviceCount: 0` | Stand-in not installed, or bridge not running | Run step 8 and start with `run-exstar.sh` |
| `usb-bridge.log` shows claim errors, or no `scanner opened` line | No permission on the USB device | Redo step 6 and check `getfacl` |
| The udev rule exists but access is unchanged | The rule was not applied to the connected device | Run the `udevadm trigger` command in step 6 |
| License error `0x16000028` | A required flash read or write failed | Check `BLOCKED` lines. Reads should show as `flash read ... st=0` |
| Calibration failed `0x02010718` | Accuracy above EXStar's limit | See calibration tips above and retry |
| A feature fails after an EXStar update | New command opcode | Look for `BLOCKED cmd` in `usb-bridge.log` |
| `install.sh` says `unexpected original DLL` | Not EXStar v1.2.2.0 | Review compatibility before changing `ORIG_SHA` |
| The launcher does nothing | EXStar is already running, or a stray process remains | Close it or run the command below |

To stop leftover processes after a crash:

```bash
pkill -f '[e]inscan_net_svr|[S]n3DprocessManager|[s]canservice\.exe|[S]nSyncService\.exe|[D]eviceHelper\.exe|[E]XStar\.exe'; pkill -x usb-bridge
```

## Logs

| Log | Location | Contents |
|---|---|---|
| USB bridge | `logs/usb-bridge.log` | Every command, status and blocked call |
| Proton | `logs/steam-exstar.log` | Wine output |
| Stand-in DLL | `<app>/syncservice/log/commshim.log` | DLL calls and connection errors |
| EXStar UI | `<app>/log/EXStar_*.log` | App events |
| Scan service | `<app>/log/scanservice_*.log` | Device and calibration results |
| Sync service | `<app>/syncservice/log/SnSyncService_*.txt` | Device reads and writes |

`<app>` is `prefix/drive_c/Shining3d/EXStar`.

**Before sharing logs**, note that EXStar's logs can contain account auth tokens,
device serial numbers and license data.

## Uninstalling

Put the original DLL back:

```bash
"$B/shim/install.sh" --restore
```

Remove the udev rule:

```bash
sudo rm /etc/udev/rules.d/70-shining3d.rules
```

Remove everything else, which **also deletes the app and any projects saved
inside the prefix**:

```bash
rm -rf "$B" ~/.local/share/applications/exstar.desktop
```

## Technical reference

All of this was worked out for interoperability with hardware the user owns.
Where a meaning is marked *inferred*, it comes from observed behaviour.

### USB

Vendor `3267`, product `0003`, interface 0, all endpoints 512 bytes.

| Endpoint | Type | Use |
|---|---|---|
| `0x01` / `0x81` | Interrupt | Commands and replies |
| `0x02` / `0x82` | Bulk | Data, including flash pages |
| `0x83` | Bulk | Image stream |

### Driver IOCTLs replaced by the bridge

| IOCTL | Purpose |
|---|---|
| `0x220000` | Get configuration descriptor |
| `0x222004` | Reset device (ignored by the stand-in) |
| `0x222008` / `0x22200C` | Write command / read reply |
| `0x222010` | Device value used by `getMaxCommandSize` |
| `0x222014`, `0x222034` | No-ops in the driver |
| `0x222018` | Read one image unit |
| `0x22201C` / `0x222020` | Stream start / stop |
| `0x222024` | Packet size, a multiple of 1024 |
| `0x222028` | Packets per unit, 1 to 10000 |
| `0x22202C` | Cached units, 1 to 10000 |
| `0x222030` | Maximum wait for a unit, in microseconds |
| `0x222038` | Unit size: `(packet size + 4) × packets per unit` |
| `0x22203C` / `0x222040` | Pipe count / pipe addresses |
| `0x222044` | Reset one pipe |

### Commands

Commands look like `seq 00 op_hi op_lo len(BE32) payload...`, and the reply echoes
bytes 0, 2 and 3 with byte 1 set to `02`. The layer's identity reads mask replies
with XOR unless byte 1 is `02`; the key is the high nibble of byte 1 repeated.

| Opcode | Meaning | Source |
|---|---|---|
| `0x0004` | Read 8-byte serial (bytes 9 to 16 of the reply) | Original layer |
| `0x0001` | Read product name (length at bytes 5 to 8, text at 9) | Original layer |
| `0x1023` | Camera exposure | EXStar log |
| `0x1027` | Camera gain | EXStar log |
| `0x1062` | Indicator | EXStar log |
| `0x1068` | LD/RGB luminance | EXStar log |
| `0x1070` | Strobe luminance | EXStar log |
| `0x1049`, `0x1041` | Trigger period / switch (*inferred*) | Connect sequence |
| `0x1057` | Read flash page: 2-byte index, in a 5120-byte buffer | EXStar log and traffic |
| `0x1058` | Write flash page: length `0x1002`, index + 4096 bytes. **Blocked** | Traffic |

Flash pages appear to be 4096 bytes (*inferred*). Observed writes: calibration at
offsets 923 (3873 bytes) and 4796 (153 bytes), activation data at 4949 (17 bytes)
and 5767 (801 bytes).

### Image stream

EXStar requests 41984-byte packets, 32 per unit, caching 100 units. Images are
1280×1024, 8-bit, from three cameras. Each USB transfer must be longer than 32
bytes and begin `3D 20`. Packets are stored with a 4-byte big-endian length. A
unit completes at its last packet or when byte 8 of a packet is `01`.

### Exported DLL notes

- The 29 exports are in namespace `CommunicateLayer`, MSVC-mangled, mapped in
  `shim/commlayer.def`.
- `std::string` by value is destroyed by the callee in the MSVC x64 ABI.
- Callers allocate through `ucrtbase`, so the stand-in must free with `ucrtbase`,
  not MinGW's `msvcrt`.
- `SHNDEVINFO` is a count followed by two 60 × 60-byte tables, serial and product
  name, 0x1C24 bytes in total.

## Disclaimer

Unofficial and not affiliated with or endorsed by Shining 3D. EXStar and EinStar
belong to their respective owners. Use at your own risk: this software talks
directly to your scanner.
