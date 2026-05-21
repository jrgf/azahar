# Azahar Switch Homebrew MVP

This directory contains a standalone Nintendo Switch homebrew MVP. It is intentionally isolated
from the main Azahar emulator build while the Switch runtime path is proven.

Exploit setup, CFW setup, firmware modification, bypass guidance, and copyrighted assets are out
of scope for this project.

## Prerequisites

- devkitPro environment variables configured.
- devkitA64 and libnx installed through `switch-dev`.
- `make` available on `PATH`.

On Unix-like systems, the Switchbrew setup flow installs the package with:

```sh
sudo dkp-pacman -S switch-dev
```

Log out and back in after installing devkitPro so `DEVKITPRO` and `DEVKITA64` are available.

## Build

From the repo root:

```sh
tools/build-switch-homebrew.sh
```

Or directly:

```sh
cd src/switch_homebrew
make
```

Expected output:

```text
src/switch_homebrew/azahar.nro
build/switch/azahar.nro
```

## Hardware Smoke Test

Copy the generated NRO to:

```text
SD:/switch/azahar.nro
```

Launch it from the Homebrew Menu. The app should show runtime status, update controller input
masks, create `SD:/switch/azahar/log.txt`, and exit cleanly when `+` is pressed.

Bootable files are scanned from:

```text
SD:/switch/azahar/roms/
```

The current Switch frontend recognizes the same file extensions as Azahar's loader:

```text
.3ds .cci .zcci .cxi .app .zcxi .cia .zcia .elf .axf .3dsx .z3dsx
```

Controls:

```text
Up/Down  Select file
A        Start Azahar boot flow
X        Rescan ROM directory
+        Exit
```

The current boot flow follows Android's native frontend sequence:

```text
validate path -> create EmuWindow -> inspect loader -> Core::System::Load -> RunLoop
```

The Switch target currently reaches the path validation step and logs the remaining bridge work.
The full `Core::System::Load` path is not linked into the Switch target yet because the Switch
`EmuWindow` and `citra_core` build target still need to be added.

## Experimental Core-Linked Target

The Android-modeled core bridge is available behind CMake option `ENABLE_SWITCH_HOMEBREW_CORE`.
It adds `EmuWindow_Switch`, links `citra_core`, probes the loader, calls `Core::System::Load`, and
runs one non-tight `RunLoop` pass.

Build command:

```sh
tools/build-switch-core-cmake.sh
```

This path requires `cmake` plus the devkitPro Switch CMake toolchain.

## Renderer Direction

The first visual milestone should use Azahar's software renderer and present its CPU framebuffer on
Switch. The accelerated path should become a proper `video_core` deko3d backend later, using the
Vulkan renderer as the architectural reference.
