# Nintendo Switch Homebrew MVP Plan

## Scope

Build a legal Nintendo Switch homebrew proof of concept for Azahar. Exploit setup, CFW setup,
firmware modification, bypass guidance, and distribution of copyrighted assets are out of scope.

This MVP is not a full emulator port. It is a standalone `.nro` that proves the Switch toolchain,
runtime lifecycle, input, filesystem access, logging, packaging, and hardware test path.

## MVP Goals

- Build an `azahar.nro` with devkitPro, devkitA64, and libnx.
- Launch from the Homebrew Menu.
- Render a simple Azahar status screen using the libnx console path.
- Poll controller input and display button state.
- Read from `sdmc:/switch/azahar/`.
- Scan bootable files from `sdmc:/switch/azahar/roms/`.
- Write a smoke-test log to `sdmc:/switch/azahar/log.txt`.
- Exit cleanly when the user presses `+`.

## Proposed Layout

```text
src/switch_homebrew/
  Makefile
  README.md
  assets/
    README.md
  source/
    emulator_bootstrap.cpp
    emulator_bootstrap.h
    emulation_session.cpp
    emulation_session.h
    main.cpp
tools/
  build-switch-homebrew.sh
  build-switch-core-cmake.sh
```

The MVP relies on the default libnx icon. A project-specific `icon.jpg` can be added later without
changing the runtime smoke test.

## Phase 1: Toolchain Skeleton

1. Add a standalone libnx Makefile under `src/switch_homebrew/`.
2. Add `main.cpp` with app init, console init, input polling, and clean shutdown.
3. Add app metadata for title, author, and version.
4. Build locally with `make` and verify `azahar.nro` is produced.

Acceptance criteria:

- Build fails clearly if `DEVKITPRO` or libnx is missing.
- `azahar.nro` is generated without touching the main Azahar CMake build.

## Phase 2: Runtime Smoke Test

1. Print app name, build version, and runtime status.
2. Display current controller state.
3. Create `sdmc:/switch/azahar/` if it does not exist.
4. Create `sdmc:/switch/azahar/roms/` if it does not exist.
5. Scan supported Azahar loader extensions in the ROM directory.
6. Append launch, boot-request, rescan, and exit events to `sdmc:/switch/azahar/log.txt`.
7. Exit cleanly on `+`.

Acceptance criteria:

- App boots from Homebrew Menu.
- Button input navigates the ROM list.
- Log file is created or a readable error is displayed.
- Pressing `A` records the selected boot request.
- Pressing `A` enters the Android-modeled boot lifecycle and records the current blocker.
- Repeated launch and exit does not crash.

## Phase 3: Project Integration

1. Add `tools/build-switch-homebrew.sh`.
2. Document macOS/Linux build prerequisites.
3. Keep Switch homebrew artifacts isolated from desktop, Android, and libretro targets.
4. Optionally add a CMake custom target later if the Makefile MVP is stable.

Acceptance criteria:

- One command builds the homebrew MVP.
- Existing Azahar targets are unchanged.
- Documentation states the legal/scope boundary.

## Test Plan

### Local Build Test

```sh
cd src/switch_homebrew
make clean
make
```

Expected result:

- Build completes.
- `azahar.nro` exists.

### SD Card Test

Copy the generated NRO to:

```text
SD:/switch/azahar.nro
```

Optional writable app directory:

```text
SD:/switch/azahar/
SD:/switch/azahar/roms/
```

Expected result:

- Homebrew Menu lists Azahar.
- App launches to the status screen.
- `+` exits cleanly.

### Hardware Smoke Matrix

- Launch with no `SD:/switch/azahar/` directory.
- Launch with an existing empty app directory.
- Launch, press several buttons, then exit.
- Launch and exit five times in a row.
- Confirm `SD:/switch/azahar/log.txt` contains launch and exit records.

## Future Port Readiness

After the MVP passes, evaluate the real emulator port separately:

- Switch `EmuWindow` implementation modeled after Android's `EmuWindow_Android`
- Switch build target that links `citra_common`, `citra_core`, `audio_core`, and renderer dependencies
- Experimental CMake build path via `ENABLE_SWITCH_HOMEBREW_CORE`
- First visual frame through the software renderer before deko3d acceleration
- Future `ENABLE_DEKO3D` renderer backend beside Vulkan/OpenGL/software
- shared core dependencies that can compile for AArch64/libnx
- frontend strategy, likely SDL2/libnx instead of Qt
- renderer feasibility
- audio backend
- input mapping
- filesystem layout
- performance and memory budget
- legal asset and key handling boundaries
