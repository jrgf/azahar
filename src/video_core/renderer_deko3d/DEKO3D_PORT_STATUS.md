# deko3d renderer — port status

Working notes for the in-progress port of the Vulkan PICA HW shader path onto
deko3d (Switch homebrew). Captures what is **demonstrably working on Switch
hardware** and what remains. Drop this file when M7 cleanup lands.

---

## Validated capabilities

Each line is a smoke-test that was observed on real `azahar-switch-libretro.nro`
running in Ryujinx, with the timestamp from `log.txt` and the value that
proved it green.

| # | Capability | Log line | Value |
|---|---|---|---|
| 1 | uam compiles GLSL → DKSH at runtime on Switch | `deko3d.uam.selftest` | `rc=0 size=512` |
| 2 | Runtime DKSH loads via `dk::ShaderMaker` | `deko3d.uam.shader-load` | `load=1` |
| 3 | Runtime DKSH is **bit-equivalent** to offline-built DKSH (`uam` CLI) | `swap=1` then frame-summary unchanged | identical path counts |
| 4 | PICA fragment shader generator + uam | `deko3d.fs-gen.smoke` | `glsl-size=6453 rc=0 dksh-size=2048` |
| 5 | Trivial PICA vertex shader generator + uam | `deko3d.vs-gen.smoke` | `glsl-size=1678 rc=0 dksh-size=768` |
| 6 | PICA FS/VS uniform structs populated each frame from live regs | `deko3d.uniforms.peek` / `.write` | `depth-scale=-1.0 write=1` |

The "M1 unknown" (cross-compile uam to aarch64-none-elf via devkitA64) is
fully resolved. Everything from here on is integration of known-good pieces.

---

## File inventory

### Vendored

- `externals/uam/` — fork of devkitPro/uam. Patched:
  - `meson.build`: distutils-free mako version check; removed
    `-ffunction-sections` (caused vtable→discarded-section issues earlier).
  - `cross-switch.txt`: meson cross-file for devkitA64. `-fvisibility=hidden`
    + `-fvisibility-inlines-hidden` so the embedded Mesa symbols don't escape
    into the final NRO and collide with anything else.
  - `source/uam_capi.{h,cpp}`: thin C ABI:
    - `int uam_compile_glsl(int stage, const char* glsl, void** out_buf, size_t* out_size)`
    - `void uam_free(void* buf)`
  - `source/compiler_iface.{h,cpp}`: new `DekoCompiler::OutputDkshToBuffer(std::vector<uint8_t>&)`
    that mirrors `OutputDksh(const char*)` but writes to memory.
  - `uam-public-symbols.txt`: exported-symbol allowlist (currently unused
    since we got `-fvisibility=hidden` working — keep for option to switch
    back to objcopy partial-link if visibility ever stops working).
- Cross-build wired into `externals/CMakeLists.txt` via `ExternalProject_Add`
  invoking meson + ninja with the devkitA64 cross-file. Result is exposed as
  an imported library target named `uam`. `find_program(BISON_BREW …)` picks
  up `/opt/homebrew/opt/bison/bin/bison` so meson's parser-gen step works on
  macOS hosts (Apple bison 2.3 is too old).
- `find_program(MESON_EXECUTABLE … REQUIRED)` and `find_program(NINJA_EXECUTABLE … REQUIRED)`
  — fail-fast if the host toolchain isn't installed.

### New (`src/video_core/renderer_deko3d/`)

| File | Role |
|---|---|
| `pica_to_deko.h` | PICA → deko enum translation tables (filter/wrap/blend/logic/compare/stencil/topology/cull/front-face). Mirrors `pica_to_vk.h`. |
| `deko_instance.{h,cpp}` | Single-`dk::Device` owner. Two constructors: own-the-device + adopt-external. Format-traits lookup by PICA pixel format / vertex attribute. |
| `deko_master_fence.{h,cpp}` | DkFence-based GPU/CPU tick sync with background waiter thread (mirrors Vulkan's `MasterSemaphoreFence`). |
| `deko_resource_pool.{h,cpp}` | `ResourcePool` base + `CommandPool` + `DescriptorHeap`, all tick-fenced for reuse. |
| `deko_scheduler.{h,cpp}` | Per-frame `dk::CmdBuf` orchestration. `Flush`/`Finish`/`Wait`. |
| `deko_stream_buffer.{h,cpp}` | `dk::MemBlock` ring allocator with watermark reclamation. |
| `deko_descriptor_update_queue.{h,cpp}` | Batched image+sampler descriptor pushes. |
| `deko_texture_runtime.{h,cpp}` | Deko-side texture runtime for `RasterizerCache`: PICA format mapping, mip-aware staging, image views, sampler translation, and custom texture uploads. |

The pool/scheduler/fence helpers still sit alongside the legacy MVP renderer.
The texture runtime is now used by batch texture binding through `RasterizerCache`.

### Modified (`renderer_deko3d.cpp`)

- `Context` now owns a `std::unique_ptr<Deko3D::Instance>` that **adopts** the
  device+queue immediately after they're created. Crucial: only one
  `dk::Device` per process is viable on Switch (M2 self-test taught us that
  the hard way — second-device init hangs).
- `Context::shader_memory_cursor` — atomic, bumped past the present shaders
  after `InitializePresentShaders`. Runtime-compiled DKSH goes after.
- `Context::pica_uniform_memory` + `pica_uniform_vs_offset` /
  `pica_uniform_fs_offset` — 256-byte-aligned UBO regions. Populated each
  frame from `RasterizerDeko3D::GetFSUniformData()` / `GetVSUniformData()`.
- `PresentBatch::texture_configs` snapshots tex0/tex1/tex2 config at draw time.
  Batch binding now asks `RasterizerDeko3D::GetTextureBinding()` for
  `RasterizerCache`-owned Deko image views and samplers instead of uploading
  guest texture bytes directly in the presenter.
- One-shot self-tests inside `RendererDeko3D::EnsureContext` and the first
  `Context::DrawPicaColorTarget` call. **These will all be deleted in M7.**

### Modified (`renderer_deko3d.h`)

- `RasterizerDeko3D::GetFSUniformData()` / `GetVSUniformData()` — public
  accessors for the protected `fs_data` / `vs_data` from
  `RasterizerAccelerated`.
- `RasterizerDeko3D::RuntimeSyncUniforms()` — public wrapper around the
  protected `SyncDrawUniforms()`.

### Build system

- `src/switch_homebrew/CMakeLists.txt`: removed the `--allow-multiple-definition`
  hack from the earlier libEGL symbol collision (the proper fix turned out to
  be **disabling `ENABLE_OPENGL` for the deko3d build**). The build is now
  configured with `-DENABLE_OPENGL=OFF`; libEGL is no longer linked.
- `src/video_core/CMakeLists.txt`: added `target_link_libraries(video_core PRIVATE … uam)`
  for the `ENABLE_DEKO3D` block.

---

## Open work (in order)

Each item is independently testable. Realistic effort and risk noted.

### Imminent — bind+draw integration (~300 LOC, **next**)

1. Add a runtime shader-pair cache. Key: `Pica::Shader::FSConfig::Hash()`.
   Value: `{DkshOffsetFS, DkshOffsetVS, dk::Shader fs, dk::Shader vs}`. Place
   DKSH bytes in `Context::shader_memory` at the cursor, bump cursor.
2. In `DrawPicaColorTarget`, replace the `present_vertex_shader` + `present_fragment_shader`
   bind with the cached PICA pair for each batch.
3. `bindUniformBuffer(DkStage_Vertex, 1, pica_uniform_memory.block.getGpuAddr() + pica_uniform_vs_offset, sizeof(VSUniformData))`
   and same for FS at binding 2.
4. Texture LUTs at bindings 3,4,5 and extra textures at 6,7 must be either
   bound to dummy buffers OR the FS must be generated with a `Profile` that
   disables lighting/proctex/shadow paths so those samplers aren't reachable.
   **Start by binding dummies** so the FS source stays close to what the
   OpenGL/Vulkan paths use — easier to A/B against.
5. After binding, the rest of the existing draw machinery (`DrawPresentVertices`)
   should "just work" because the vertex attribute layout already matches
   the trivial VS's expectations (the trivial VS takes the same vertex
   layout the present shader did — `in_position`/`in_color`/`in_texcoord0` etc.).

**Failure modes to expect on first boot:**
- GPU hang from unbound LUT texture buffers — fix: bind dummies (step 4).
- Black/garbage pixels from `std140` padding mismatch — fix: cross-check
  `FSUniformData`'s `alignas(16)` placements match `std140` in the generated
  GLSL. The header has `static_assert(sizeof == 0x530)` so wire-size matches
  the GLSL `layout(std140)` block.
- Crash from invalid vertex attrib state — already debugged; the trivial VS
  consumes the same `PresentVertexAttribState` we already set up.

### Medium-term — correctness iteration (multiple loops)

- LUTs (lighting / proctex) — needs `samplerBuffer` GPU memory populated
  from `regs.lighting.luts` / `regs.texturing.proctex_*`. Without them,
  lighting-heavy scenes look wrong; without them *and* without dummies bound,
  the GPU hangs.
- Texture binding for tex0/tex1/tex2 now goes through the shared cache. Remaining
  texture work is format/mip parity, render-target-as-texture invalidation,
  custom texture edge cases, and unsupported texture types.
- Depth/stencil — currently `pica_color_target` has no depth buffer. PICA's
  alpha test + depth test live in the FS, but the rasterizer side also needs
  state.
- Per-batch render state — blend, cull, scissor, viewport — currently
  hard-coded in `DrawPresentVertices`. Needs translation from PICA regs
  via `pica_to_deko.h`.

### Long-term — M3-M7 proper

The newer infrastructure files in M2 (`deko_scheduler`, `deko_master_fence`,
`deko_resource_pool`, `deko_stream_buffer`, `deko_descriptor_update_queue`) are
still mostly outside the rendering path. The current renderer uses ad-hoc
`DekoMemBlock` / `dk::CmdBuf` directly. The next architectural pass replaces
the ad-hoc code with the new abstractions and continues closing
`vk_texture_runtime` parity gaps.

---

## How to verify state

```sh
DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64 \
  cmake --build build-switch-libretro-deko-cmake \
        --target azahar-switch-libretro-nro -j8

# Expected outputs:
ls -la build-switch-libretro-deko-cmake/bin/azahar-switch-libretro.nro
#   ~ 15.4 MB
ls -la build-switch-libretro-deko-cmake/externals/uam-build/libuam.a
#   ~ 6 MB, ELF aarch64 ar archive

# Run in Ryujinx (or copy NRO to a Switch with HBL). Watch the log:
tail -f /Volumes/<sd-mount>/switch/azahar/log.txt    # on Switch
# or
cat build-switch-libretro-deko-cmake/bin/log.txt     # Ryujinx leaves it here
```

The log lines listed in the **Validated capabilities** table above should all
appear within the first 5 seconds of boot. If any are missing, that's the
regression starting point.

## Lessons baked into the build (so they don't recur)

- **Apple bison 2.3 is too old** for uam — pin to brew's `/opt/homebrew/opt/bison/bin` in `find_program`.
- **`distutils.version` was removed in Python 3.12+** — uam's meson.build was patched.
- **Static-library symbol hiding** requires either visibility attributes OR
  objcopy+partial-link. The objcopy path tripped over function-sections /
  COMDAT in vtables; visibility is cleaner.
- **`ENABLE_OPENGL=OFF`** is mandatory for `ENABLE_DEKO3D=ON` builds while
  uam is embedded — libEGL ships its own Mesa stack with conflicting symbols.
  The fix is a build-system decision, not a code workaround.
- **One `dk::Device` per process** — the M2 self-test that created a second
  one to verify the new infrastructure hung the boot indefinitely.
  `Deko3D::Instance` now has an "adopt external device" constructor for this
  exact reason.
