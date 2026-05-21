# Switch Renderer Strategy

## Goal

Use Azahar's existing renderer architecture for the Nintendo Switch port. The Vulkan backend is the
main reference for the accelerated path, but deko3d should become a first-class backend later rather
than an ad hoc translation layer inside the homebrew launcher.

## Renderer Boundary

The correct boundary is `video_core`:

- `video_core/renderer_base.h`
- `video_core/rasterizer_interface.h`
- existing backends:
  - `renderer_software`
  - `renderer_opengl`
  - `renderer_vulkan`

The Switch frontend should not convert Vulkan calls directly. It should provide `EmuWindow_Switch`
and let `video_core` choose a renderer backend.

## Milestones

1. Core-linked headless boot:
   - Build `azahar-switch-core`.
   - Call `Loader::GetLoader`.
   - Call `Core::System::Load`.
   - Run one `RunLoop(false)` pass.

2. First visual frame:
   - Use `RendererSoftware` first.
   - Copy `SwRenderer::ScreenInfo` pixels into a libnx/deko3d-presented surface.
   - This is expected to be slow, but it proves boot, frame production, and presentation.

3. deko3d backend:
   - Add `video_core/renderer_deko3d/`.
   - Implement a `RendererBase` subclass beside `RendererVulkan`.
   - Implement a deko3d rasterizer/cache path modeled from Vulkan concepts.
   - Port shader and pipeline handling deliberately; do not one-to-one rewrite Vulkan calls in place.

## Current State

- deko3d is installed under devkitPro:
  - `/opt/devkitpro/libnx/include/deko3d.h`
  - `/opt/devkitpro/libnx/include/deko3d.hpp`
  - `/opt/devkitpro/libnx/lib/libdeko3d.a`
- `ENABLE_SWITCH_HOMEBREW_CORE` adds the experimental core-linked Switch target.
- `ENABLE_DEKO3D` is reserved for the future accelerated renderer backend.
