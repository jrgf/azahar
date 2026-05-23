// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "video_core/rasterizer_accelerated.h"
#include "video_core/renderer_deko3d/deko_texture_runtime.h"
#include "video_core/renderer_base.h"
#include "video_core/pica/regs_texturing.h"
#include "video_core/shader/generator/pica_fs_config.h"
#include "video_core/shader/generator/shader_uniforms.h"


namespace Core {
class System;
}

namespace Pica {
class PicaCore;
struct MemoryFillConfig;
}

namespace VideoCore {
class CustomTexManager;
}

namespace Deko3D {

constexpr std::size_t PicaTextureUnitCount = 3;

struct PresentVertex {
    std::array<float, 4> position{};
    std::array<float, 4> color{};
    std::array<float, 2> tex_coord0{};
    std::array<float, 2> tex_coord1{};
    std::array<float, 2> tex_coord2{};
    float tex_coord0_w = 1.0f;
    std::array<float, 4> normquat{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> view{0.0f, 0.0f, 1.0f};
};

struct PresentTextureInfo {
    PAddr address = 0;
    u32 width = 0;
    u32 height = 0;
    u32 format = 0;
    u32 type = 0;
    u32 mag_filter = 1;
    u32 min_filter = 1;
    u32 mip_filter = 0;
    u32 wrap_s = 0;
    u32 wrap_t = 0;
    u32 border_color = 0;
    bool enabled = false;
};

struct PresentTextureConfig {
    bool enabled = false;
    Pica::TexturingRegs::TextureConfig config{};
    Pica::TexturingRegs::TextureFormat format{};
};

struct TextureBinding {
    dk::ImageView image_view;
    dk::Sampler sampler;
    bool valid = false;
};

struct PresentBatch {
    std::vector<PresentVertex> vertices;
    PresentTextureInfo texture;
    std::array<PresentTextureInfo, PicaTextureUnitCount> textures{};
    std::array<PresentTextureConfig, PicaTextureUnitCount> texture_configs{};
    /// Framebuffer target that was active when this batch was emitted. The
    /// Deko present path replays queued batches later, so it must not collapse
    /// top/bottom/intermediate PICA targets into the framebuffer that happens
    /// to be current at SwapBuffers time.
    PAddr target_color_address = 0;
    PAddr target_depth_address = 0;
    u32 target_width = 0;
    u32 target_height = 0;
    u32 target_color_format = 0;
    bool target_enabled = false;
    /// Snapshot of the PICA fragment-shader configuration that was live when
    /// the rasterizer emitted this batch. Lets the renderer compile + bind
    /// the matching runtime FS per-batch rather than reusing one FS for the
    /// whole frame (which is wrong when batches have different TEV / alpha
    /// test / blend state). std::optional because FSConfig has no default
    /// constructor — it's built from PICA regs.
    std::optional<Pica::Shader::FSConfig> fs_config;
    u64 fs_config_hash = 0;
    /// Per-batch uniform snapshots. Each batch may run with different TEV
    /// const_color / tev_combiner_buffer_color / blend_color / lod bias /
    /// border color / lighting values; binding a single frame-wide UBO
    /// gives wrong output for any batch whose state differs from the last
    /// one. Snapshot at the rasterizer level and rebind per batch.
    Pica::Shader::Generator::FSUniformData fs_uniform_data{};
    Pica::Shader::Generator::VSUniformData vs_uniform_data{};
    /// Per-batch render-state snapshot (PICA output_merger). Without this,
    /// alpha-only / IA4 / I4 textures render as opaque white because the
    /// renderer falls back to a default no-blend state for every batch.
    struct RenderState {
        bool blend_enable = false;
        u32 blend_eq_rgb = 0;       // BlendEquation
        u32 blend_eq_a = 0;
        u32 src_factor_rgb = 1;     // BlendFactor (1 = One)
        u32 dst_factor_rgb = 0;     // BlendFactor (0 = Zero)
        u32 src_factor_a = 1;
        u32 dst_factor_a = 0;
        bool depth_test_enable = false;
        u32 depth_test_func = 1;    // CompareFunc (1 = Always)
        bool depth_write_enable = false;
        u8 color_write_mask = 0xF;  // R|G|B|A
        u32 logic_op = 3;           // LogicOp::Copy
        u32 blend_const = 0;
        // Stencil state — per Mikage retrospective, SM3DL's environment
        // rendering uses stencil masking. Without these wired correctly,
        // the background draws as flat white panels.
        bool stencil_enable = false;
        u32 stencil_test_func = 1;     // CompareFunc (1 = Always)
        u8 stencil_ref = 0;
        u8 stencil_input_mask = 0xFF;
        u8 stencil_write_mask = 0xFF;
        u32 stencil_action_fail = 0;       // StencilAction::Keep
        u32 stencil_action_depth_fail = 0;
        u32 stencil_action_depth_pass = 0;
        u32 cull_mode = 0;          // RasterizerRegs::CullMode::KeepAll
        bool flip_viewport = false;
        bool viewport_valid = false;
        s32 viewport_x = 0;
        s32 viewport_y = 0;
        s32 viewport_width = 0;
        s32 viewport_height = 0;
        u32 scissor_x = 0;
        u32 scissor_y = 0;
        u32 scissor_width = 0;
        u32 scissor_height = 0;
    } render_state{};
};

struct DisplayTransferRecord {
    PAddr input_address = 0;
    PAddr output_address = 0;
    u32 input_width = 0;
    u32 input_height = 0;
    u32 output_width = 0;
    u32 output_height = 0;
    u32 input_format = 0;
    u32 output_format = 0;
    u32 flags = 0;
    u32 scaling = 0;
    PAddr target_address = 0;
    u32 target_width = 0;
    u32 target_height = 0;
    u32 target_color_format = 0;
    u32 batch_count = 0;
};

/// Interface the RendererDeko3D::Context exposes so the rasterizer can
/// submit a PICA batch to the GPU immediately during DrawTriangles —
/// mirroring vk_rasterizer's Draw()-per-call architecture. The previous
/// model deferred everything until SwapBuffers and replayed all batches
/// into one render target, which collapsed multi-pass scenes (3D logo
/// behind UI panels, layered title screens) onto a single layer.
class BatchSubmitter {
public:
    virtual ~BatchSubmitter() = default;
    /// Submit a single batch. The implementation owns lazy frame setup
    /// (binding render target, starting cmd buffer) — the rasterizer just
    /// hands over the batch. Default no-op so types can inherit without
    /// implementing this yet (rolling refactor).
    virtual void SubmitBatch(const PresentBatch& batch) {
        (void)batch;
    }
    virtual u32 SubmittedBatchCount() const {
        return 0;
    }
};

class RasterizerDeko3D final : public VideoCore::RasterizerAccelerated {
public:
    explicit RasterizerDeko3D(Memory::MemorySystem& memory, Pica::PicaCore& pica,
                              VideoCore::CustomTexManager& custom_tex_manager,
                              VideoCore::RendererBase& renderer);
    ~RasterizerDeko3D() override;

    void SetDekoContext(dk::Device device, dk::Queue queue, dk::CmdBuf command_buffer);
    /// Wire the submitter that DrawTriangles will push completed batches to.
    /// When set, the rasterizer submits each batch immediately. When null,
    /// it falls back to the legacy snapshot-then-replay path (consumed via
    /// ConsumePresentBatches in the swap path).
    void SetBatchSubmitter(BatchSubmitter* submitter) {
        batch_submitter = submitter;
    }
    void DrawTriangles() override;
    void FlushAll() override;
    void FlushRegion(PAddr addr, u32 size) override;
    void InvalidateRegion(PAddr addr, u32 size) override;
    void FlushAndInvalidateRegion(PAddr addr, u32 size) override;
    void ClearAll(bool flush) override;
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateFill(const Pica::MemoryFillConfig& config) override;
    bool AccelerateDrawBatch(bool is_indexed) override;
    TextureBinding GetTextureBinding(const PresentTextureConfig& texture);
    std::vector<DisplayTransferRecord> ConsumeDisplayTransfers();
    std::vector<PresentBatch> ConsumePresentBatches();

    /// Snapshot of the PICA fragment-shader uniforms last computed by
    /// RasterizerAccelerated::SyncDrawUniforms(). Read-only — the renderer
    /// copies these into a GPU UBO each frame so the runtime-compiled FS
    /// can read them.
    [[nodiscard]] const Pica::Shader::Generator::FSUniformData& GetFSUniformData() const noexcept {
        return fs_data;
    }
    [[nodiscard]] const Pica::Shader::Generator::VSUniformData& GetVSUniformData() const noexcept {
        return vs_data;
    }

    /// Public wrapper around RasterizerAccelerated::SyncDrawUniforms — lets
    /// the renderer refresh PICA uniforms from current register state right
    /// before consuming GetFSUniformData()/GetVSUniformData().
    void RuntimeSyncUniforms() {
        SyncDrawUniforms();
    }

private:
    u32 draw_batch_log_count = 0;
    u32 fallback_draw_log_count = 0;
    u32 render_target_log_count = 0;
    u32 texture_sample_log_count = 0;
    u32 display_transfer_log_count = 0;
    TextureRuntime texture_runtime;
    RasterizerCache res_cache;
    std::vector<DisplayTransferRecord> display_transfers;
    std::vector<PresentBatch> present_batches;
    BatchSubmitter* batch_submitter = nullptr;
};

class RendererDeko3D final : public VideoCore::RendererBase {
public:
    explicit RendererDeko3D(Core::System& system, Pica::PicaCore& pica,
                            Frontend::EmuWindow& window);
    ~RendererDeko3D() override;

    [[nodiscard]] VideoCore::RasterizerInterface* Rasterizer() override {
        return &rasterizer;
    }

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override {}

private:
    struct Context;

    bool EnsureContext();

    Memory::MemorySystem& memory;
    Pica::PicaCore& pica;
    RasterizerDeko3D rasterizer;
    std::unique_ptr<Context> context;
    u32 frame_count = 0;
    bool init_failed = false;
};

} // namespace Deko3D
