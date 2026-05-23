// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

namespace OpenGL {

// One-shot probe: verifies that Switch-Mesa supports creating a shared EGL
// context, making it current on a worker thread, and compiling/linking a
// trivial program whose handle is visible from the main thread. Idempotent —
// safe to call multiple times; only executes the probe once. Results are
// emitted to the Switch flow log under stage=opengl.async.probe.*.
void RunSharedContextProbe();

} // namespace OpenGL
