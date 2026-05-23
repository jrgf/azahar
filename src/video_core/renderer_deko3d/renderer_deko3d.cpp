// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_deko3d/renderer_deko3d.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <deko3d.hpp>

#include "deko_present_fsh_dksh.h"
#include "deko_present_vsh_dksh.h"

#include "common/color.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/memory.h"
#include "video_core/custom_textures/custom_tex_manager.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_external.h"
#include "video_core/pica/regs_framebuffer.h"

#include "video_core/renderer_deko3d/deko_instance.h"
#include "video_core/renderer_deko3d/pica_to_deko.h"
#include "video_core/renderer_deko3d/deko_scheduler.h"
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/glsl_shader_gen.h"
#include "video_core/shader/generator/pica_fs_config.h"
#include "video_core/shader/generator/profile.h"

#include "uam_capi.h"

extern "C" {
struct NWindow;
NWindow* nwindowGetDefault(void);
}

#ifdef __SWITCH__
namespace Azahar::Switch {
bool AppendLogFormat(int* error_out, const char* format, ...);
}
#endif

namespace Deko3D {
namespace {

constexpr u32 FramebufferWidth = 1280;
constexpr u32 FramebufferHeight = 720;
constexpr u32 NumFramebuffers = 2;
constexpr u32 CommandMemorySize = 4 * 1024 * 1024;
// Primary shader memory holds the offline present shaders plus the bootstrap
// runtime PICA VS/FS pair. Runtime FS variants are allocated in separate
// 256 KB chunks below so we avoid the one-large-code-memblock hang observed
// with a 4 MB shader block, while still letting the FS cache grow past the
// ~82 variants that fit in the primary block.
constexpr u32 ShaderMemorySize = 256 * 1024;
constexpr u32 PicaShaderChunkSize = 256 * 1024;
constexpr u32 MaxPicaShaderChunks = 8;
/// Master switch for the experimental "runtime-compiled PICA shaders bound
/// for pica-target draws" path. When false, the renderer behaves exactly as
/// the MVP (present_vsh+fsh for every quad). When true, EnsurePicaRuntimeShaders
/// is invoked and the runtime pair is bound via the override mechanism.
/// Keep this in one place so we can flip back on regression.
constexpr bool ExperimentalRuntimePicaShaders = true;
/// Debug-only: replace the generated PICA FS with a trivial passthrough
/// (out_color = texture(tex0, texcoord0)). v69 bisect confirmed textures
/// upload + sample correctly with this on, so the remaining "white"
/// issue was downstream (TEV chain or vs_data.flip_viewport double-flip).
/// Keep this off for correctness runs. The simple FS is useful only to prove
/// raw texture sampling; it intentionally bypasses TEV/color/alpha logic and
/// makes many valid batches render as white.
constexpr bool DebugForceSimpleFS = false;
/// Kill switch for the v78 depth attachment. Real-hardware Tegra fails to
/// boot the NRO with `DkImageFormat_ZF32_X24S8 + HwCompression`. Set to
/// false to bisect: if hardware boots → depth setup is the regression.
/// Ryujinx tolerates either way; the visual will lose depth-tested
/// occlusion when off, which is fine for 2D-only UI scenes.
constexpr bool EnableDepthAttachment = true;
/// Bind PICA LUT samplerBuffers (lighting / fog / proctex). Hardware is the
/// source of truth: generated PICA fragment shaders declare slots 3-7, so we
/// must bind valid descriptors even if Ryujinx's host backend dislikes the
/// DkImageType_Buffer path.
constexpr bool EnableLutBinding = true;
/// Ryujinx can crash its host Vulkan backend on async texture readback, but
/// hardware is the source of truth. Keep this off unless explicitly bisecting
/// emulator-only queue timing.
constexpr bool DebugWaitIdleAfterFrameSubmit = false;
/// Master switch for heavyweight diagnostic dumps (full FS-GLSL chunks,
/// per-variant FS dumps, alpha-patch byte dumps). These emit 30+ KB of
/// log writes per frame which hardware SD storage can't keep up with —
/// suspected cause of the v81 hardware truncation around frame 175.
/// Keep on for Ryujinx investigation; off for hardware runs.
constexpr bool EnableHeavyDiagnostics = false;
constexpr std::size_t MaxPresentVertices = 192 * 1024;
constexpr u32 PresentVertexBufferSize = static_cast<u32>(MaxPresentVertices * sizeof(PresentVertex));
constexpr u32 TextureDescriptorSlots = 8;
constexpr u32 LutLfElementCount = Pica::LightingRegs::NumLightingSampler * 256 + 128;
constexpr u32 LutRgElementCount = 128 * 3;
constexpr u32 LutRgbaElementCount = 256 * 2;
constexpr float PresentClipPadding = 2.0f;
constexpr float PresentOverlayAlpha = 0.35f;
constexpr bool PresentDebugOverlay = false;
constexpr bool PresentDebugMirrorBottom = false;
constexpr bool DekoHotTrace = false;
constexpr bool DekoFrameSummaryTrace = true;

constexpr std::array<DkVtxAttribState, 3> PresentVertexAttribState{{
    {0, 0, static_cast<u32>(offsetof(PresentVertex, position)), DkVtxAttribSize_4x32,
     DkVtxAttribType_Float, 0},
    {0, 0, static_cast<u32>(offsetof(PresentVertex, color)), DkVtxAttribSize_4x32,
     DkVtxAttribType_Float, 0},
    {0, 0, static_cast<u32>(offsetof(PresentVertex, tex_coord0)), DkVtxAttribSize_2x32,
     DkVtxAttribType_Float, 0},
}};

constexpr std::array<DkVtxBufferState, 1> PresentVertexBufferState{{
    {sizeof(PresentVertex), 0},
}};

constexpr std::array<DkVtxAttribState, 8> PicaVertexAttribState{{
    {0, 0, static_cast<u32>(offsetof(PresentVertex, position)), DkVtxAttribSize_4x32,
     DkVtxAttribType_Float, 0},
    {0, 0, static_cast<u32>(offsetof(PresentVertex, color)), DkVtxAttribSize_4x32,
     DkVtxAttribType_Float, 0},
    {0, 0, static_cast<u32>(offsetof(PresentVertex, tex_coord0)), DkVtxAttribSize_2x32,
     DkVtxAttribType_Float, 0},
    {0, 0, static_cast<u32>(offsetof(PresentVertex, tex_coord1)), DkVtxAttribSize_2x32,
     DkVtxAttribType_Float, 0},
    {0, 0, static_cast<u32>(offsetof(PresentVertex, tex_coord2)), DkVtxAttribSize_2x32,
     DkVtxAttribType_Float, 0},
    {0, 0, static_cast<u32>(offsetof(PresentVertex, tex_coord0_w)), DkVtxAttribSize_1x32,
     DkVtxAttribType_Float, 0},
    {0, 0, static_cast<u32>(offsetof(PresentVertex, normquat)), DkVtxAttribSize_4x32,
     DkVtxAttribType_Float, 0},
    {0, 0, static_cast<u32>(offsetof(PresentVertex, view)), DkVtxAttribSize_3x32,
     DkVtxAttribType_Float, 0},
}};

constexpr std::array<DkVtxBufferState, 1> PicaVertexBufferState{{
    {sizeof(PresentVertex), 0},
}};

#ifdef __SWITCH__
bool ShouldTraceDekoFrame(u32 frame) {
    return DekoHotTrace && (frame < 8 || frame == 16 || frame == 32 || (frame % 60) == 0);
}

bool ShouldTraceDekoFrameSummary(u32 frame) {
    return DekoFrameSummaryTrace &&
           (frame < 4 || frame == 8 || frame == 16 || (frame % 120) == 0);
}

bool IsTraceTextureFormat(Pica::TexturingRegs::TextureFormat format) {
    using Format = Pica::TexturingRegs::TextureFormat;
    switch (format) {
    case Format::IA8:
    case Format::RG8:
    case Format::I8:
    case Format::A8:
    case Format::IA4:
    case Format::I4:
    case Format::A4:
    case Format::ETC1:
    case Format::ETC1A4:
        return true;
    default:
        return false;
    }
}

u32 TextureAddress(const PresentTextureConfig& texture) {
    return texture.enabled ? static_cast<u32>(texture.config.GetPhysicalAddress()) : 0;
}

u32 TextureWidth(const PresentTextureConfig& texture) {
    return texture.enabled ? static_cast<u32>(texture.config.width.Value()) : 0;
}

u32 TextureHeight(const PresentTextureConfig& texture) {
    return texture.enabled ? static_cast<u32>(texture.config.height.Value()) : 0;
}

bool IsSupportedDekoSamplerConfig(const Pica::TexturingRegs::TextureConfig& config) {
    using TextureConfig = Pica::TexturingRegs::TextureConfig;
    const auto valid_filter = [](TextureConfig::TextureFilter filter) {
        switch (filter) {
        case TextureConfig::Nearest:
        case TextureConfig::Linear:
            return true;
        default:
            return false;
        }
    };
    const auto valid_wrap = [](TextureConfig::WrapMode wrap) {
        return static_cast<u32>(wrap) <= static_cast<u32>(TextureConfig::Repeat3);
    };

    return valid_filter(config.mag_filter.Value()) && valid_filter(config.min_filter.Value()) &&
           valid_filter(config.mip_filter.Value()) && valid_wrap(config.wrap_s.Value()) &&
           valid_wrap(config.wrap_t.Value());
}

bool ShouldTraceTextureBinding(const PresentTextureConfig& texture) {
    if (!DekoHotTrace || !texture.enabled || !IsTraceTextureFormat(texture.format)) {
        return false;
    }
    static std::atomic_uint trace_count{0};
    return trace_count.fetch_add(1, std::memory_order_relaxed) < 512;
}

bool ShouldTraceBatchStack(const PresentBatch& batch) {
    if (!DekoHotTrace) {
        return false;
    }
    bool risky_texture = false;
    for (const PresentTextureConfig& texture : batch.texture_configs) {
        risky_texture |= texture.enabled && IsTraceTextureFormat(texture.format);
    }
    if (!risky_texture && batch.vertices.size() < 256) {
        return false;
    }
    static std::atomic_uint trace_count{0};
    return trace_count.fetch_add(1, std::memory_order_relaxed) < 512;
}

struct TexCoordRange {
    float min_u = 0.0f;
    float max_u = 0.0f;
    float min_v = 0.0f;
    float max_v = 0.0f;
};

const std::array<float, 2>& GetTexCoord(const PresentVertex& vertex, std::size_t unit) {
    switch (unit) {
    case 1:
        return vertex.tex_coord1;
    case 2:
        return vertex.tex_coord2;
    default:
        return vertex.tex_coord0;
    }
}

TexCoordRange GetTexCoordRange(const PresentBatch& batch, std::size_t unit) {
    if (batch.vertices.empty()) {
        return {};
    }

    TexCoordRange range{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
    };

    for (const PresentVertex& vertex : batch.vertices) {
        const auto& coord = GetTexCoord(vertex, unit);
        range.min_u = std::min(range.min_u, coord[0]);
        range.max_u = std::max(range.max_u, coord[0]);
        range.min_v = std::min(range.min_v, coord[1]);
        range.max_v = std::max(range.max_v, coord[1]);
    }

    return range;
}

void LogBatchStack(const char* step, u32 frame, u32 batch_index, const PresentBatch& batch,
                   int line) {
    const auto& t0 = batch.texture_configs[0];
    const auto& t1 = batch.texture_configs[1];
    const auto& t2 = batch.texture_configs[2];
    const auto& s0 = batch.textures[0];
    const auto& s1 = batch.textures[1];
    const auto& s2 = batch.textures[2];
    const TexCoordRange uv0 = GetTexCoordRange(batch, 0);
    const TexCoordRange uv1 = GetTexCoordRange(batch, 1);
    const TexCoordRange uv2 = GetTexCoordRange(batch, 2);
    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=deko3d.stack step=%s func=DrawPicaColorTarget line=%d "
        "frame=%u batch=%u vertices=%u "
        "fs=%016llX depth=%u/%u color-mask=%u scissor=%d,%d,%d,%d "
        "uv0=%f..%f/%f..%f uv1=%f..%f/%f..%f uv2=%f..%f/%f..%f "
        "s0=%u/%u/%u/%u/%u s1=%u/%u/%u/%u/%u s2=%u/%u/%u/%u/%u "
        "t0=%u:%08X:%ux%u:%u:%u t1=%u:%08X:%ux%u:%u:%u t2=%u:%08X:%ux%u:%u:%u",
        step, line, frame, batch_index, static_cast<u32>(batch.vertices.size()),
        static_cast<unsigned long long>(batch.fs_config_hash),
        batch.render_state.depth_test_enable ? 1 : 0,
        batch.render_state.depth_write_enable ? 1 : 0,
        static_cast<u32>(batch.render_state.color_write_mask),
        batch.fs_uniform_data.scissor_x1, batch.fs_uniform_data.scissor_y1,
        batch.fs_uniform_data.scissor_x2, batch.fs_uniform_data.scissor_y2, uv0.min_u,
        uv0.max_u, uv0.min_v, uv0.max_v, uv1.min_u, uv1.max_u, uv1.min_v,
        uv1.max_v, uv2.min_u, uv2.max_u, uv2.min_v, uv2.max_v, s0.wrap_s, s0.wrap_t,
        s0.mag_filter, s0.min_filter, s0.mip_filter, s1.wrap_s, s1.wrap_t, s1.mag_filter,
        s1.min_filter, s1.mip_filter, s2.wrap_s, s2.wrap_t, s2.mag_filter, s2.min_filter,
        s2.mip_filter,
        t0.enabled ? 1 : 0, TextureAddress(t0), TextureWidth(t0), TextureHeight(t0),
        static_cast<u32>(t0.format), static_cast<u32>(t0.config.type.Value()),
        t1.enabled ? 1 : 0, TextureAddress(t1), TextureWidth(t1), TextureHeight(t1),
        static_cast<u32>(t1.format), static_cast<u32>(t1.config.type.Value()),
        t2.enabled ? 1 : 0, TextureAddress(t2), TextureWidth(t2), TextureHeight(t2),
        static_cast<u32>(t2.format), static_cast<u32>(t2.config.type.Value()));
}
#endif

bool IsFiniteVertex(const PresentVertex& vertex) {
    for (float value : vertex.position) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    for (float value : vertex.color) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

std::vector<PresentVertex> BuildDrawableVertices(const std::vector<PresentVertex>& vertices,
                                                 float alpha_cap, bool rotate_clockwise) {
    std::vector<PresentVertex> drawable;
    drawable.reserve(vertices.size());

    for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
        std::array<PresentVertex, 3> tri{vertices[i], vertices[i + 1], vertices[i + 2]};
        bool valid = true;
        float min_x = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        float min_y = std::numeric_limits<float>::max();
        float max_y = std::numeric_limits<float>::lowest();

        for (auto& vertex : tri) {
            if (rotate_clockwise) {
                const float x = vertex.position[0];
                const float y = vertex.position[1];
                vertex.position[0] = y;
                vertex.position[1] = -x;
            }
            if (!IsFiniteVertex(vertex) || std::abs(vertex.position[3]) < 0.000001f) {
                valid = false;
                break;
            }

            const float x = vertex.position[0] / vertex.position[3];
            const float y = vertex.position[1] / vertex.position[3];
            if (!std::isfinite(x) || !std::isfinite(y)) {
                valid = false;
                break;
            }

            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            for (float& color : vertex.color) {
                color = std::clamp(color, 0.0f, 1.0f);
            }
            vertex.color[3] = std::min(vertex.color[3], alpha_cap);
        }

        if (!valid || max_x < -PresentClipPadding || min_x > PresentClipPadding ||
            max_y < -PresentClipPadding || min_y > PresentClipPadding) {
            continue;
        }

        drawable.insert(drawable.end(), tri.begin(), tri.end());
    }

    return drawable;
}

u32 AlignUp(u32 value, u32 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

PresentTextureInfo GetTextureInfo(const Pica::TexturingRegs::FullTextureConfig& texture,
                                   std::size_t unit) {
    const bool has_texture = texture.enabled != 0 && texture.config.address != 0 &&
                             texture.config.width.Value() != 0 &&
                             texture.config.height.Value() != 0;
    const auto texture_type =
        unit == 0 ? texture.config.type.Value() : Pica::TexturingRegs::TextureConfig::Texture2D;
    return {
        texture.config.GetPhysicalAddress(),
        static_cast<u32>(texture.config.width.Value()),
        static_cast<u32>(texture.config.height.Value()),
        static_cast<u32>(texture.format),
        static_cast<u32>(texture_type),
        static_cast<u32>(texture.config.mag_filter.Value()),
        static_cast<u32>(texture.config.min_filter.Value()),
        static_cast<u32>(texture.config.mip_filter.Value()),
        static_cast<u32>(texture.config.wrap_s.Value()),
        static_cast<u32>(texture.config.wrap_t.Value()),
        texture.config.border_color.raw,
        has_texture,
    };
}

PresentTextureConfig GetTextureConfig(const Pica::TexturingRegs::FullTextureConfig& texture,
                                      std::size_t unit) {
    Pica::TexturingRegs::TextureConfig config = texture.config;
    if (unit != 0) {
        config.type.Assign(Pica::TexturingRegs::TextureConfig::Texture2D);
    }
    return {
        .enabled = texture.enabled != 0 && config.address != 0 && config.width.Value() != 0 &&
                   config.height.Value() != 0,
        .config = config,
        .format = texture.format,
    };
}

std::array<PresentTextureInfo, PicaTextureUnitCount> GetTextureInfos(
    const Pica::RegsInternal& regs) {
    std::array<PresentTextureInfo, PicaTextureUnitCount> infos{};
    const auto textures = regs.texturing.GetTextures();
    for (std::size_t unit = 0; unit < infos.size(); ++unit) {
        infos[unit] = GetTextureInfo(textures[unit], unit);
    }
    return infos;
}

std::array<PresentTextureConfig, PicaTextureUnitCount> GetTextureConfigs(
    const Pica::RegsInternal& regs) {
    std::array<PresentTextureConfig, PicaTextureUnitCount> configs{};
    const auto textures = regs.texturing.GetTextures();
    for (std::size_t unit = 0; unit < configs.size(); ++unit) {
        configs[unit] = GetTextureConfig(textures[unit], unit);
    }
    return configs;
}

PresentTextureInfo GetTexture0Info(const Pica::RegsInternal& regs) {
    return GetTextureInfos(regs)[0];
}

struct DekoMemBlock {
    dk::MemBlock block{};
    u32 size = 0;

    ~DekoMemBlock() {
        Destroy();
    }

    DekoMemBlock() = default;
    DekoMemBlock(const DekoMemBlock&) = delete;
    DekoMemBlock& operator=(const DekoMemBlock&) = delete;

    bool Create(dk::Device device, u32 requested_size, u32 flags) {
        Destroy();
        size = AlignUp(std::max(requested_size, static_cast<u32>(DK_MEMBLOCK_ALIGNMENT)),
                       static_cast<u32>(DK_MEMBLOCK_ALIGNMENT));
        block = dk::MemBlockMaker{device, size}.setFlags(flags).create();
        return static_cast<bool>(block);
    }

    void Destroy() {
        if (block) {
            block.destroy();
        }
        size = 0;
    }
};

struct DekoScreenFrame {
    std::vector<u8> pixels;
    Common::Rectangle<u32> dst{};
    PAddr framebuffer_addr = 0;
    u32 width = 0;
    u32 height = 0;
    bool enabled = false;
    bool has_rgb = false;
};

struct DekoPicaTargetInfo {
    PAddr color_address = 0;
    u32 width = 0;
    u32 height = 0;
    /// PICA framebuffer.color_format raw value (Pica::FramebufferRegs::ColorFormat).
    /// Mikage's first SM3DL fix was RGB565 display transfer handling — using the
    /// wrong format here corrupts post-PICA blits and produces blue-strip /
    /// off-color artifacts in the present output.
    u32 color_format = 0;
    bool enabled = false;
};

struct DekoSourceImage {
    dk::Image image{};
    DekoMemBlock image_memory{};
    DekoMemBlock staging_memory{};
    u32 width = 0;
    u32 height = 0;
    bool ready = false;

    bool Ensure(dk::Device device, dk::Queue queue, u32 new_width, u32 new_height) {
        if (width == new_width && height == new_height && ready && staging_memory.block) {
            return true;
        }

        queue.waitIdle();
        image = {};
        image_memory.Destroy();
        staging_memory.Destroy();
        width = new_width;
        height = new_height;
        ready = false;

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(width, height)
            .initialize(layout);

        if (!image_memory.Create(device, layout.getSize(),
                                 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                     DkMemBlockFlags_Image)) {
            return false;
        }
        image.initialize(layout, image_memory.block, 0);

        const u32 staging_size = width * height * 4;
        ready = staging_memory.Create(device, staging_size,
                                      DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        return ready;
    }

    bool Upload(const DekoScreenFrame& screen) {
        const u32 byte_size = width * height * 4;
        void* dst = staging_memory.block ? staging_memory.block.getCpuAddr() : nullptr;
        if (dst == nullptr || screen.pixels.size() < byte_size) {
            return false;
        }
        std::memcpy(dst, screen.pixels.data(), byte_size);
        staging_memory.block.flushCpuCache(0, byte_size);
        return true;
    }
};

struct DekoDescriptorSet {
    DekoMemBlock memory{};
    u32 descriptor_size = 0;
    u32 count = 0;

    bool Ensure(dk::Device device, u32 new_count, u32 new_descriptor_size) {
        if (memory.block && count == new_count && descriptor_size == new_descriptor_size) {
            return true;
        }
        count = new_count;
        descriptor_size = new_descriptor_size;
        return memory.Create(device, count * descriptor_size,
                             DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    }

    void BindForImages(dk::CmdBuf command_buffer) {
        command_buffer.bindImageDescriptorSet(memory.block.getGpuAddr(), count);
    }

    void BindForSamplers(dk::CmdBuf command_buffer) {
        command_buffer.bindSamplerDescriptorSet(memory.block.getGpuAddr(), count);
    }

    template <typename Descriptor>
    void Update(dk::CmdBuf command_buffer, u32 index, const Descriptor& descriptor) {
        command_buffer.pushData(memory.block.getGpuAddr() + index * descriptor_size, &descriptor,
                                descriptor_size);
    }
};

struct DekoTextureImage {
    dk::Image image{};
    DekoMemBlock image_memory{};
    DekoMemBlock staging_memory{};
    PresentTextureInfo info{};
    bool ready = false;
    bool uploaded = false;

    bool EnsureWhite(dk::Device device, dk::Queue queue) {
        PresentTextureInfo white_info{};
        white_info.address = 0;
        white_info.width = 1;
        white_info.height = 1;
        white_info.format = 0;
        white_info.type = 0;
        white_info.enabled = true;
        if (ready && info.width == 1 && info.height == 1 && info.address == 0) {
            return true;
        }

        queue.waitIdle();
        image = {};
        image_memory.Destroy();
        staging_memory.Destroy();
        info = white_info;
        ready = false;
        uploaded = false;

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(1, 1)
            .initialize(layout);

        if (!image_memory.Create(device, layout.getSize(),
                                 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                     DkMemBlockFlags_Image)) {
            return false;
        }
        image.initialize(layout, image_memory.block, 0);

        if (!staging_memory.Create(device, 4,
                                   DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)) {
            return false;
        }
        auto* dst = static_cast<u8*>(staging_memory.block.getCpuAddr());
        dst[0] = 255;
        dst[1] = 255;
        dst[2] = 255;
        dst[3] = 255;
        staging_memory.block.flushCpuCache(0, 4);
        ready = true;
        return true;
    }

    bool UploadIfNeeded(dk::CmdBuf command_buffer) {
        if (uploaded) {
            return false;
        }
        const DkCopyBuf upload_buffer{staging_memory.block.getGpuAddr(), 0, 0};
        dk::ImageView image_view{image};
        const DkImageRect image_rect{0, 0, 0, info.width, info.height, 1};
        command_buffer.copyBufferToImage(upload_buffer, image_view, image_rect);
        uploaded = true;
        return true;
    }
};

struct DekoBufferTexture {
    dk::Image image{};
    DekoMemBlock memory{};
    DkImageFormat format = DkImageFormat_None;
    u32 element_count = 0;
    bool ready = false;

    bool Ensure(dk::Device device, dk::Queue queue, DkImageFormat new_format, u32 new_element_count,
                const void* initial_data, u32 initial_data_size) {
        if (ready && memory.block && format == new_format && element_count == new_element_count) {
            return true;
        }

        queue.waitIdle();
        image = {};
        memory.Destroy();
        format = new_format;
        element_count = new_element_count;
        ready = false;

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            .setType(DkImageType_Buffer)
            .setFormat(format)
            .setDimensions(element_count)
            .initialize(layout);

        if (!memory.Create(device, layout.getSize(),
                           DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                               DkMemBlockFlags_Image)) {
            return false;
        }
        image.initialize(layout, memory.block, 0);

        auto* dst = static_cast<u8*>(memory.block.getCpuAddr());
        if (dst == nullptr) {
            return false;
        }
        std::memset(dst, 0, memory.size);
        if (initial_data != nullptr && initial_data_size != 0) {
            std::memcpy(dst, initial_data, std::min(initial_data_size, memory.size));
        }
        memory.block.flushCpuCache(0, memory.size);
        ready = true;
        return true;
    }

    bool Upload(const void* data, u32 data_size) {
        if (!ready || !memory.block || data == nullptr || data_size > memory.size) {
            return false;
        }
        auto* dst = static_cast<u8*>(memory.block.getCpuAddr());
        if (dst == nullptr) {
            return false;
        }
        std::memcpy(dst, data, data_size);
        memory.block.flushCpuCache(0, data_size);
        return true;
    }
};

struct DekoRenderTarget {
    dk::Image image{};
    DekoMemBlock image_memory{};
    PAddr address = 0;
    u32 width = 0;
    u32 height = 0;
    bool ready = false;

    /// Active deko3d format for the color target. We track it so subsequent
    /// presents/blits know how to interpret the pixel data. PICA's
    /// ColorFormat is independent of the deko3d format we allocate; mismatches
    /// produced the "blue-strip-over-white" artifacts in Mario 3D Land
    /// (Mikage's first SM3DL fix).
    DkImageFormat color_format = DkImageFormat_RGBA8_Unorm;

    static DkImageFormat MapColorFormat(u32 pica_color_format) {
        using P = Pica::FramebufferRegs::ColorFormat;
        switch (static_cast<P>(pica_color_format)) {
        case P::RGB8:    return DkImageFormat_RGBX8_Unorm;
        case P::RGB5A1:  return DkImageFormat_RGB5A1_Unorm;
        case P::RGB565:  return DkImageFormat_RGB565_Unorm;
        case P::RGBA4:   return DkImageFormat_RGBA4_Unorm;
        case P::RGBA8:
        default:         return DkImageFormat_RGBA8_Unorm;
        }
    }

    static DkImageFormat MapPixelFormat(Pica::PixelFormat pixel_format) {
        switch (pixel_format) {
        case Pica::PixelFormat::RGB8:   return DkImageFormat_RGBX8_Unorm;
        case Pica::PixelFormat::RGB565: return DkImageFormat_RGB565_Unorm;
        case Pica::PixelFormat::RGB5A1: return DkImageFormat_RGB5A1_Unorm;
        case Pica::PixelFormat::RGBA4:  return DkImageFormat_RGBA4_Unorm;
        case Pica::PixelFormat::RGBA8:
        default:                        return DkImageFormat_RGBA8_Unorm;
        }
    }

    bool Ensure(dk::Device device, dk::Queue queue, PAddr new_address, u32 new_width,
                u32 new_height, u32 pica_color_format = 0) {
        return EnsureDkFormat(device, queue, new_address, new_width, new_height,
                              MapColorFormat(pica_color_format));
    }

    bool EnsureDkFormat(dk::Device device, dk::Queue queue, PAddr new_address, u32 new_width,
                        u32 new_height, DkImageFormat desired_format) {
        if (address == new_address && width == new_width && height == new_height &&
            color_format == desired_format && ready) {
            return true;
        }

        queue.waitIdle();
        image = {};
        image_memory.Destroy();
        address = new_address;
        width = new_width;
        height = new_height;
        color_format = desired_format;
        ready = false;

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
            .setFormat(color_format)
            .setDimensions(width, height)
            .initialize(layout);

        if (!image_memory.Create(device, layout.getSize(),
                                 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                     DkMemBlockFlags_Image)) {
            return false;
        }
        image.initialize(layout, image_memory.block, 0);
        ready = true;
        return true;
    }

    /// Allocate a depth/stencil image with the same dims as a color target.
    /// `address` is reused as a synthetic key (we use the color target's
    /// address since PICA pairs a depth buffer to each color buffer).
    bool EnsureDepth(dk::Device device, dk::Queue queue, PAddr new_key, u32 new_width,
                     u32 new_height) {
        if (address == new_key && width == new_width && height == new_height && ready) {
            return true;
        }
        queue.waitIdle();
        image = {};
        image_memory.Destroy();
        address = new_key;
        width = new_width;
        height = new_height;
        ready = false;

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            // Z24S8: depth + stencil packed into 32 bits. PICA games that
            // use stencil masking (Mario 3D Land's environment rendering
            // per Mikage's retrospective) need a stencil channel. Avoid
            // the float-depth + HwCompression combo that hit a Tegra
            // landmine in v78 — stick to Z24S8 unorm without compression.
            .setFlags(DkImageFlags_UsageRender)
            .setFormat(DkImageFormat_Z24S8)
            .setDimensions(width, height)
            .initialize(layout);

        if (!image_memory.Create(device, layout.getSize(),
                                 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                     DkMemBlockFlags_Image)) {
            return false;
        }
        image.initialize(layout, image_memory.block, 0);
        ready = true;
        return true;
    }
};

} // namespace

struct RendererDeko3D::Context : public BatchSubmitter {
    dk::UniqueDevice device{};
    dk::UniqueQueue queue{};
    dk::UniqueCmdBuf command_buffer{};
    /// Adopts `device` + `queue` once Initialize() succeeds. Gives the new
    /// Deko3D::* infrastructure (TextureRuntime, Scheduler, etc. in upcoming
    /// increments) access to the same single dk::Device the legacy MVP uses,
    /// avoiding the double-device hang we saw during M2 selftest.
    std::unique_ptr<Deko3D::Instance> instance{};
    dk::UniqueSwapchain swapchain{};
    DekoMemBlock command_memory{};
    DekoMemBlock shader_memory{};
    DekoMemBlock present_vertex_memory{};
    std::array<dk::Image, NumFramebuffers> framebuffers{};
    std::array<DekoMemBlock, NumFramebuffers> framebuffer_memory{};
    DekoSourceImage top_source{};
    DekoSourceImage bottom_source{};
    DekoRenderTarget pica_color_target{};
    /// Depth/stencil pair for pica_color_target. Required for PICA depth
    /// testing/writing — without it, depth_test_enable in the snapshot is
    /// meaningless and any draw with depth_test=true sees no buffer.
    DekoRenderTarget pica_depth_target{};
    std::unordered_map<PAddr, DekoRenderTarget> pica_color_targets{};
    std::unordered_map<PAddr, DekoRenderTarget> pica_depth_targets{};
    std::unordered_map<PAddr, DekoRenderTarget> display_targets{};
    std::vector<DisplayTransferRecord> cached_display_transfers{};
    DekoDescriptorSet image_descriptor_set{};
    DekoDescriptorSet sampler_descriptor_set{};
    DekoTextureImage white_texture{};
    DekoBufferTexture lut_lf_fallback{};
    DekoBufferTexture lut_rg_fallback{};
    DekoBufferTexture lut_rgba_fallback{};
    dk::Shader present_vertex_shader{};
    dk::Shader present_fragment_shader{};
    u32 present_vertex_capacity = 0;
    u32 present_vertex_frame_offset = 0;
    u32 present_draw_log_count = 0;
    u32 present_nonempty_log_count = 0;
    u32 target_render_log_count = 0;
    u32 lut_upload_log_count = 0;
    u32 pica_vtx_bind_log_count = 0;
    u32 variant_log_count = 0;
    u32 transfer_pick_log_count = 0;
    std::vector<PresentBatch> cached_present_batches;
    u32 cached_present_age = 0;
    u32 cached_display_transfer_age = 0;
    u32 pica_compile_frame = std::numeric_limits<u32>::max();
    u32 pica_compiles_this_frame = 0;
    /// Next free byte (aligned) inside `shader_memory`. Bumped past the
    /// present shaders during InitializePresentShaders so runtime-compiled
    /// PICA HW shaders can be placed after.
    std::atomic<u32> shader_memory_cursor{0};
    /// Shared memblock backing the PICA fragment + vertex uniform buffers.
    /// Layout: [vs_data (256-byte aligned)] [fs_data (256-byte aligned)].
    /// Each frame we memcpy the latest contents and flushCpuCache before any
    /// future draw consumes them.
    DekoMemBlock pica_uniform_memory{};
    u32 pica_uniform_vs_offset = 0;
    u32 pica_uniform_fs_offset = 0;
    u32 pica_uniform_vs_size = 0;
    u32 pica_uniform_fs_size = 0;
    /// Distance (in bytes) between two consecutive slots inside
    /// `pica_uniform_memory`. Each slot stores one [vs_data, fs_data] pair.
    u32 pica_uniform_slot_stride = 0;
    /// Number of slots reserved per frame for per-batch UBO snapshots.
    /// Must be >= the worst-case batch count we observe (logs show ~360).
    static constexpr u32 MaxUniformSlots = 1024;
    /// Single-entry runtime PICA shader pair (legacy single-cache; future
    /// cache uses the map below).
    dk::Shader pica_runtime_vs{};
    dk::Shader pica_runtime_fs{};
    bool pica_runtime_ready = false;
    u64 pica_runtime_fs_hash = 0;

    /// Multi-entry runtime PICA shader cache keyed on FSConfig::Hash().
    /// Stores compiled DKSH in chunked code memblocks and a dk::Shader wrapper
    /// for each unique configuration encountered.
    struct CachedPicaShader {
        dk::Shader fs{};
        u32 fs_offset = 0;
        u32 fs_size = 0;
    };
    struct PicaShaderAllocation {
        DekoMemBlock* memory = nullptr;
        u32 offset = 0;
    };
    std::unordered_map<u64, CachedPicaShader> pica_shader_cache;
    std::vector<std::unique_ptr<DekoMemBlock>> pica_shader_memory_chunks;
    u32 pica_shader_chunk_cursor = 0;
    /// When non-null, DrawPresentVertices uses these instead of the offline
    /// present_*_shader values. Set per-batch around DrawPicaColorTarget,
    /// cleared after, so the final swapchain-blit pass still goes through
    /// the present shaders.
    const dk::Shader* override_vertex_shader = nullptr;
    const dk::Shader* override_fragment_shader = nullptr;
    /// Most recently bound shader pair. DrawPresentVertices skips the
    /// bindShaders call when these match the requested pair, avoiding
    /// expensive pipeline-state switches when consecutive batches share
    /// the same FSConfig.
    const dk::Shader* last_bound_vs = nullptr;
    const dk::Shader* last_bound_fs = nullptr;

    ~Context() {
        if (queue) {
            queue.waitIdle();
        }
    }

    bool Initialize() {
        // Reserve enough buckets so insertions never trigger a rehash —
        // pointers we hand out via GetOrCompilePicaFS must remain valid for
        // the lifetime of the Context. Without this, growing the cache past
        // its load factor invalidates `&entry.fs` pointers we already
        // stashed in command-buffer state, causing a GPU/CPU crash a few
        // hundred frames in.
        pica_shader_cache.reserve(2048);

        device = dk::DeviceMaker{}.create();
        if (!device) {
            LOG_CRITICAL(Render, "Deko3D device creation failed");
            return false;
        }

        queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();
        if (!queue) {
            LOG_CRITICAL(Render, "Deko3D queue creation failed");
            return false;
        }

        // Hand the just-created device+queue to a Deko3D::Instance so the new
        // Deko3D::* abstractions can share the single dk::Device this renderer
        // owns. Strict adoption (no double-device init).
        instance = std::make_unique<Deko3D::Instance>(dk::Device(device), dk::Queue(queue));

        command_buffer = dk::CmdBufMaker{device}.create();
        if (!command_buffer) {
            LOG_CRITICAL(Render, "Deko3D command buffer creation failed");
            return false;
        }

        if (!command_memory.Create(device, CommandMemorySize,
                                   DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)) {
            LOG_CRITICAL(Render, "Deko3D command memory creation failed");
            return false;
        }

        if (!shader_memory.Create(device, ShaderMemorySize,
                                  DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                      DkMemBlockFlags_Code)) {
            LOG_CRITICAL(Render, "Deko3D shader memory creation failed");
            return false;
        }

        if (!InitializePresentShaders()) {
            LOG_CRITICAL(Render, "Deko3D present shader initialization failed");
            return false;
        }

        // Allocate the PICA uniform memblock. Layout per slot:
        //   [vs_data (aligned)] [fs_data (aligned)]
        // We reserve MaxUniformSlots slots so DrawPicaColorTarget can give
        // each batch its own UBO offset (TEV const_color, blend, lod bias,
        // etc. change per batch and a single frame-wide UBO produces wrong
        // output for any batch whose state differs from the last one).
        pica_uniform_vs_size = sizeof(Pica::Shader::Generator::VSUniformData);
        pica_uniform_fs_size = sizeof(Pica::Shader::Generator::FSUniformData);
        pica_uniform_slot_stride = AlignUp(
            AlignUp(pica_uniform_vs_size, DK_UNIFORM_BUF_ALIGNMENT) +
                pica_uniform_fs_size,
            DK_UNIFORM_BUF_ALIGNMENT);
        // Legacy single-slot offsets (kept for any old call sites that
        // haven't migrated yet).
        pica_uniform_vs_offset = 0;
        pica_uniform_fs_offset =
            AlignUp(pica_uniform_vs_offset + pica_uniform_vs_size, DK_UNIFORM_BUF_ALIGNMENT);
        const u32 uniform_total = AlignUp(
            pica_uniform_slot_stride * MaxUniformSlots,
            DK_MEMBLOCK_ALIGNMENT);
        if (!pica_uniform_memory.Create(device, uniform_total,
                                         DkMemBlockFlags_CpuUncached |
                                             DkMemBlockFlags_GpuCached)) {
            LOG_CRITICAL(Render, "Deko3D PICA uniform memory creation failed");
            return false;
        }

        std::vector<Common::Vec2f> lut_lf_neutral(LutLfElementCount, {1.0f, 0.0f});
        std::vector<Common::Vec2f> lut_rg_neutral(LutRgElementCount, {1.0f, 0.0f});
        std::vector<Common::Vec4f> lut_rgba_neutral(LutRgbaElementCount,
                                                    {1.0f, 1.0f, 1.0f, 1.0f});
        if (!image_descriptor_set.Ensure(device, TextureDescriptorSlots, sizeof(DkImageDescriptor)) ||
            !sampler_descriptor_set.Ensure(device, TextureDescriptorSlots, sizeof(DkSamplerDescriptor)) ||
            !white_texture.EnsureWhite(device, queue) ||
            !lut_lf_fallback.Ensure(device, queue, DkImageFormat_RG32_Float, LutLfElementCount,
                                    lut_lf_neutral.data(),
                                    static_cast<u32>(lut_lf_neutral.size() *
                                                     sizeof(lut_lf_neutral[0]))) ||
            !lut_rg_fallback.Ensure(device, queue, DkImageFormat_RG32_Float, LutRgElementCount,
                                    lut_rg_neutral.data(),
                                    static_cast<u32>(lut_rg_neutral.size() *
                                                     sizeof(lut_rg_neutral[0]))) ||
            !lut_rgba_fallback.Ensure(device, queue, DkImageFormat_RGBA32_Float,
                                      LutRgbaElementCount, lut_rgba_neutral.data(),
                                      static_cast<u32>(lut_rgba_neutral.size() *
                                                       sizeof(lut_rgba_neutral[0])))) {
            LOG_CRITICAL(Render, "Deko3D texture descriptor initialization failed");
            return false;
        }

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                      DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(FramebufferWidth, FramebufferHeight)
            .initialize(layout);

        std::array<DkImage const*, NumFramebuffers> images{};
        for (u32 i = 0; i < NumFramebuffers; ++i) {
            if (!framebuffer_memory[i].Create(device, layout.getSize(),
                                              DkMemBlockFlags_CpuUncached |
                                                  DkMemBlockFlags_GpuCached |
                                                  DkMemBlockFlags_Image)) {
                LOG_CRITICAL(Render, "Deko3D framebuffer memory creation failed");
                return false;
            }
            framebuffers[i].initialize(layout, framebuffer_memory[i].block, 0);
            images[i] = &framebuffers[i];
        }

        swapchain = dk::SwapchainMaker{device, nwindowGetDefault(), images}.create();
        if (!swapchain) {
            LOG_CRITICAL(Render, "Deko3D swapchain creation failed");
            return false;
        }

        LOG_INFO(Render, "android-flow stage=switch-renderer result=deko3d-native");
        return true;
    }

    bool LoadEmbeddedShader(dk::Shader& shader, const u8* data, std::size_t size, u32& offset) {
        const u32 aligned_offset = AlignUp(offset, DK_SHADER_CODE_ALIGNMENT);
        const u32 aligned_size = AlignUp(static_cast<u32>(size), DK_SHADER_CODE_ALIGNMENT);
        if (aligned_offset + aligned_size > shader_memory.size) {
            return false;
        }

        auto* code = static_cast<u8*>(shader_memory.block.getCpuAddr());
        std::memcpy(code + aligned_offset, data, size);
        shader_memory.block.flushCpuCache(aligned_offset, aligned_size);
        dk::ShaderMaker{shader_memory.block, aligned_offset}.initialize(shader);
        offset = aligned_offset + aligned_size;
        return true;
    }

    bool InitializePresentShaders() {
        u32 code_offset = 0;
        const bool ok =
            LoadEmbeddedShader(present_vertex_shader, deko_present_vsh_dksh,
                               deko_present_vsh_dksh_size, code_offset) &&
            LoadEmbeddedShader(present_fragment_shader, deko_present_fsh_dksh,
                               deko_present_fsh_dksh_size, code_offset);
        if (ok) {
            shader_memory_cursor.store(code_offset, std::memory_order_relaxed);
        }
        return ok;
    }

    /// On first call: generate the trivial PICA VS + the PICA FS from
    /// `live_regs`, compile both via uam, place the DKSH bytes into
    /// `shader_memory`, initialise `pica_runtime_vs` / `pica_runtime_fs`.
    /// Subsequent calls early-out unless the FSConfig hash differs from the
    /// cached one — in which case the FS is regenerated (in-place reuse of
    /// the same shader_memory slot for now; the proper hash-keyed cache
    /// lands later). Returns true when both shaders are usable.
    bool EnsurePicaRuntimeShaders(const Pica::RegsInternal& live_regs) {
        Pica::Shader::FSConfig fs_config{live_regs};
        const u64 fs_hash = fs_config.Hash();
        if (pica_runtime_ready && pica_runtime_fs_hash == fs_hash) {
            return true;
        }

        Pica::Shader::UserConfig user_config{};
        Pica::Shader::Profile profile{};
        profile.has_separable_shaders = 1;
        profile.has_clip_planes = 1;
        profile.has_geometry_shader = 1;
        profile.has_custom_border_color = 1;
        profile.has_logic_op = 1;
        // deko3d uses Vulkan-style [0,1] depth range; with =1 the FS would
        // assume OpenGL [-1,1] and compute gl_FragDepth values outside the
        // valid Vulkan range. Result: depth values clamp/collapse and the
        // depth test rejects overlay batches.
        profile.has_minus_one_to_one_range = 0;
        profile.is_vulkan = 0;

        std::string vs_glsl =
            Pica::Shader::Generator::GLSL::GenerateTrivialVertexShader(
                /*use_clip_planes=*/false, /*separable_shader=*/true);
        vs_glsl.insert(0, "#version 460 core\n");

        std::string fs_glsl;
        if (DebugForceSimpleFS) {
            fs_glsl = R"(#version 460 core
#extension GL_ARB_separate_shader_objects : enable
layout(location = 0) in vec4 primary_color;
layout(location = 1) in vec2 texcoord0;
layout(location = 2) in vec2 texcoord1;
layout(location = 3) in vec2 texcoord2;
layout(location = 4) in float texcoord0_w;
layout(location = 5) in vec4 normquat;
layout(location = 6) in vec3 view;
layout(binding = 0) uniform sampler2D tex0;
layout(location = 0) out vec4 color;
void main() {
    color = texture(tex0, texcoord0);
}
)";
        } else {
            // Unmodified PICA FS. The scissor patch above populates the UBO so
            // the FS's scissor discard now works on real bounds instead of
            // discarding every fragment.
            fs_glsl = Pica::Shader::Generator::GLSL::GenerateFragmentShader(fs_config, user_config,
                                                                             profile);
            fs_glsl.insert(0, "#version 460 core\n");
        }

        // Dump the FULL FS GLSL once, in 1.5KB chunks. The TEV chain plus
        // bindings span ~6KB total — the tail-only dump was missing stage 0
        // (the most informative one). Each chunk is logged under
        // deko3d.fs-gen.chunk with index/total so we can reassemble.
        static std::atomic_bool fs_full_dumped{false};
        bool full_expected = false;
        if (EnableHeavyDiagnostics &&
            fs_full_dumped.compare_exchange_strong(full_expected, true)) {
            constexpr std::size_t kChunk = 1500;
            const std::size_t total =
                (fs_glsl.size() + kChunk - 1) / kChunk;
            for (std::size_t i = 0; i < total; ++i) {
                const std::size_t offset = i * kChunk;
                const std::size_t len = std::min(kChunk, fs_glsl.size() - offset);
                std::string chunk(fs_glsl, offset, len);
                for (auto& c : chunk) {
                    if (c == '\n' || c == '\r') c = '|';
                    if (c == '"') c = '\'';
                }
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.fs-gen.chunk i=%zu of=%zu "
                    "total-size=%zu chunk-size=%zu body=\"%s\"",
                    i, total, fs_glsl.size(), len, chunk.c_str());
            }
        }

        void* vs_dksh = nullptr;
        size_t vs_dksh_size = 0;
        const int vs_rc =
            uam_compile_glsl(UAM_STAGE_VERTEX, vs_glsl.c_str(), &vs_dksh, &vs_dksh_size);

        void* fs_dksh = nullptr;
        size_t fs_dksh_size = 0;
        const int fs_rc =
            uam_compile_glsl(UAM_STAGE_FRAGMENT, fs_glsl.c_str(), &fs_dksh, &fs_dksh_size);

        bool ok = false;
        if (vs_rc == 0 && fs_rc == 0 && vs_dksh && fs_dksh) {
            // For now we always reuse the same two slots: bump cursor once,
            // place VS, then FS. Future cache will key by hash.
            const u32 cursor_start = shader_memory_cursor.load(std::memory_order_relaxed);
            const u32 vs_off =
                AlignUp(cursor_start, static_cast<u32>(DK_SHADER_CODE_ALIGNMENT));
            const u32 vs_sz =
                AlignUp(static_cast<u32>(vs_dksh_size),
                        static_cast<u32>(DK_SHADER_CODE_ALIGNMENT));
            const u32 fs_off = vs_off + vs_sz;
            const u32 fs_sz =
                AlignUp(static_cast<u32>(fs_dksh_size),
                        static_cast<u32>(DK_SHADER_CODE_ALIGNMENT));
            if (fs_off + fs_sz <= shader_memory.size) {
                auto* code = static_cast<u8*>(shader_memory.block.getCpuAddr());
                std::memcpy(code + vs_off, vs_dksh, vs_dksh_size);
                std::memcpy(code + fs_off, fs_dksh, fs_dksh_size);
                shader_memory.block.flushCpuCache(vs_off, fs_off + fs_sz - vs_off);

                dk::ShaderMaker{shader_memory.block, vs_off}.initialize(pica_runtime_vs);
                dk::ShaderMaker{shader_memory.block, fs_off}.initialize(pica_runtime_fs);

                shader_memory_cursor.store(fs_off + fs_sz, std::memory_order_relaxed);
                pica_runtime_ready = true;
                pica_runtime_fs_hash = fs_hash;
                ok = true;
            }
        }

        if (vs_dksh) uam_free(vs_dksh);
        if (fs_dksh) uam_free(fs_dksh);
        return ok;
    }

    /// Multi-entry FS cache lookup. Compiles the PICA FS for the given
    /// FSConfig if not already cached, places its DKSH bytes in
    /// chunked PICA shader arena, and returns a pointer to the cached entry. The single
    /// trivial VS owned by EnsurePicaRuntimeShaders is reused across all
    /// FS variants.
    PicaShaderAllocation AllocatePicaShaderCode(u32 aligned_size) {
        if (aligned_size == 0 || aligned_size > PicaShaderChunkSize) {
            return {};
        }

        auto allocate_chunk = [&]() -> bool {
            if (pica_shader_memory_chunks.size() >= MaxPicaShaderChunks) {
                return false;
            }
            auto chunk = std::make_unique<DekoMemBlock>();
            if (!chunk->Create(device, PicaShaderChunkSize,
                               DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                   DkMemBlockFlags_Code)) {
                return false;
            }
#ifdef __SWITCH__
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.pica-shader-chunk.new index=%u size=%u",
                static_cast<u32>(pica_shader_memory_chunks.size()), chunk->size);
#endif
            pica_shader_memory_chunks.emplace_back(std::move(chunk));
            pica_shader_chunk_cursor = 0;
            return true;
        };

        const auto has_space = [&]() {
            if (pica_shader_memory_chunks.empty()) {
                return false;
            }
            const u32 offset =
                AlignUp(pica_shader_chunk_cursor, static_cast<u32>(DK_SHADER_CODE_ALIGNMENT));
            return offset + aligned_size <= pica_shader_memory_chunks.back()->size;
        };

        if (!has_space() && !allocate_chunk()) {
            return {};
        }

        const u32 offset =
            AlignUp(pica_shader_chunk_cursor, static_cast<u32>(DK_SHADER_CODE_ALIGNMENT));
        pica_shader_chunk_cursor = offset + aligned_size;
        return {pica_shader_memory_chunks.back().get(), offset};
    }

    CachedPicaShader* GetOrCompilePicaFS(const Pica::Shader::FSConfig& fs_config,
                                          u64 fs_hash) {
        if (auto it = pica_shader_cache.find(fs_hash); it != pica_shader_cache.end()) {
            return &it->second;
        }
        Pica::Shader::UserConfig user_config{};
        Pica::Shader::Profile profile{};
        profile.has_separable_shaders = 1;
        profile.has_clip_planes = 1;
        profile.has_geometry_shader = 1;
        profile.has_custom_border_color = 1;
        profile.has_logic_op = 1;
        // deko3d uses Vulkan-style [0,1] depth range; with =1 the FS would
        // assume OpenGL [-1,1] and compute gl_FragDepth values outside the
        // valid Vulkan range. Result: depth values clamp/collapse and the
        // depth test rejects overlay batches.
        profile.has_minus_one_to_one_range = 0;
        profile.is_vulkan = 0;

        std::string fs_glsl;
        if (DebugForceSimpleFS) {
            fs_glsl = R"(#version 460 core
#extension GL_ARB_separate_shader_objects : enable
layout(location = 0) in vec4 primary_color;
layout(location = 1) in vec2 texcoord0;
layout(location = 2) in vec2 texcoord1;
layout(location = 3) in vec2 texcoord2;
layout(location = 4) in float texcoord0_w;
layout(location = 5) in vec4 normquat;
layout(location = 6) in vec3 view;
layout(binding = 0) uniform sampler2D tex0;
layout(location = 0) out vec4 color;
void main() {
    color = texture(tex0, texcoord0);
}
)";
        } else {
            fs_glsl = Pica::Shader::Generator::GLSL::GenerateFragmentShader(fs_config, user_config,
                                                                             profile);
            fs_glsl.insert(0, "#version 460 core\n");
        }

#ifdef __SWITCH__
        // Dump the GLSL of the first few unique FSConfig variants. The
        // very first batch's chain renders Mario correctly; the white
        // batches use a different FSConfig and we need to see that GLSL
        // to find the stage that collapses to (1,1,1).
        constexpr u32 MaxFSVariantsLogged = 4;
        if (EnableHeavyDiagnostics && variant_log_count < MaxFSVariantsLogged) {
            const u32 variant_idx = variant_log_count++;
            constexpr std::size_t kChunk = 1500;
            const std::size_t total =
                (fs_glsl.size() + kChunk - 1) / kChunk;
            for (std::size_t i = 0; i < total; ++i) {
                const std::size_t offset = i * kChunk;
                const std::size_t len = std::min(kChunk, fs_glsl.size() - offset);
                std::string chunk(fs_glsl, offset, len);
                for (auto& c : chunk) {
                    if (c == '\n' || c == '\r') c = '|';
                    if (c == '"') c = '\'';
                }
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.fs-variant variant=%u hash=%016llx "
                    "i=%zu of=%zu total-size=%zu body=\"%s\"",
                    variant_idx,
                    static_cast<unsigned long long>(fs_hash),
                    i, total, fs_glsl.size(), chunk.c_str());
            }
        }
#endif

        void* fs_dksh = nullptr;
        size_t fs_dksh_size = 0;
        const int fs_rc =
            uam_compile_glsl(UAM_STAGE_FRAGMENT, fs_glsl.c_str(), &fs_dksh, &fs_dksh_size);
        if (fs_rc != 0 || !fs_dksh) {
#ifdef __SWITCH__
            static std::atomic_uint compile_fail_log_count{0};
            if (compile_fail_log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.pica-shader-compile.fail "
                    "func=RendererDeko3D::GetOrCompilePicaFS line=%u hash=%016llX "
                    "rc=%d has-dksh=%u dksh-size=%zu glsl-size=%zu cache-size=%zu",
                    static_cast<u32>(__LINE__), static_cast<unsigned long long>(fs_hash), fs_rc,
                    fs_dksh ? 1U : 0U, fs_dksh_size, fs_glsl.size(),
                    pica_shader_cache.size());
            }
#endif
            if (fs_dksh) uam_free(fs_dksh);
            return nullptr;
        }
        const u32 fs_sz =
            AlignUp(static_cast<u32>(fs_dksh_size),
                    static_cast<u32>(DK_SHADER_CODE_ALIGNMENT));
        const PicaShaderAllocation allocation = AllocatePicaShaderCode(fs_sz);
        if (allocation.memory == nullptr) {
#ifdef __SWITCH__
            static std::atomic_uint full_log_count{0};
            if (full_log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.pica-shader-cache.full "
                    "func=RendererDeko3D::GetOrCompilePicaFS line=%u hash=%016llX "
                    "fs-size=%u chunks=%u cursor=%u cache-size=%zu",
                    static_cast<u32>(__LINE__), static_cast<unsigned long long>(fs_hash), fs_sz,
                    static_cast<u32>(pica_shader_memory_chunks.size()),
                    pica_shader_chunk_cursor, pica_shader_cache.size());
            }
#endif
            uam_free(fs_dksh);
            return nullptr;
        }
        auto* code = static_cast<u8*>(allocation.memory->block.getCpuAddr());
        std::memcpy(code + allocation.offset, fs_dksh, fs_dksh_size);
        allocation.memory->block.flushCpuCache(allocation.offset, fs_sz);

        auto [it, inserted] = pica_shader_cache.try_emplace(fs_hash);
        auto& cached = it->second;
        cached.fs_offset = allocation.offset;
        cached.fs_size = fs_sz;
        dk::ShaderMaker{allocation.memory->block, allocation.offset}.initialize(cached.fs);
#ifdef __SWITCH__
        static std::atomic_uint insert_log_count{0};
        if (insert_log_count.fetch_add(1, std::memory_order_relaxed) < 128) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.pica-shader-cache.insert "
                "func=RendererDeko3D::GetOrCompilePicaFS line=%u hash=%016llX "
                "fs-size=%u offset=%u chunks=%u cursor=%u cache-size=%zu",
                static_cast<u32>(__LINE__), static_cast<unsigned long long>(fs_hash), fs_sz,
                allocation.offset, static_cast<u32>(pica_shader_memory_chunks.size()),
                pica_shader_chunk_cursor, pica_shader_cache.size());
        }
#endif
        uam_free(fs_dksh);
        return &cached;
    }

    bool PrepareSource(DekoSourceImage& source, const DekoScreenFrame& screen) {
        if (!screen.enabled || screen.width == 0 || screen.height == 0 || screen.pixels.empty()) {
            return true;
        }
        return source.Ensure(device, queue, screen.width, screen.height) && source.Upload(screen);
    }

    bool EnsurePresentVertexBuffer(u32 byte_size) {
        if (byte_size == 0) {
            return true;
        }
        if (present_vertex_memory.block && byte_size <= present_vertex_capacity) {
            return true;
        }

        queue.waitIdle();
        if (!present_vertex_memory.Create(device, byte_size,
                                          DkMemBlockFlags_CpuUncached |
                                              DkMemBlockFlags_GpuCached)) {
            present_vertex_capacity = 0;
            return false;
        }
        present_vertex_capacity = present_vertex_memory.size;
        return true;
    }

    void UpdateImageDescriptor(u32 slot, dk::Image& image, DkImageType view_type = DkImageType_None) {
        dk::ImageView image_view{image};
        if (view_type != DkImageType_None) {
            image_view.setType(view_type);
        }
        dk::ImageDescriptor image_descriptor;
        image_descriptor.initialize(image_view);
        image_descriptor_set.Update(command_buffer, slot, image_descriptor);
    }

    void UpdateSamplerDescriptor(u32 slot, const dk::Sampler& sampler) {
        dk::SamplerDescriptor sampler_descriptor;
        sampler_descriptor.initialize(sampler);
        sampler_descriptor_set.Update(command_buffer, slot, sampler_descriptor);
    }

    void BindLutFallbackDescriptors(std::array<DkResHandle, TextureDescriptorSlots>& handles) {
        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Nearest, DkFilter_Nearest);
        sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
        sampler.setLodClamp(0.0f, 0.0f);

        UpdateImageDescriptor(3, lut_lf_fallback.image, DkImageType_Buffer);
        UpdateSamplerDescriptor(3, sampler);
        handles[3] = dkMakeTextureHandle(3, 3);

        UpdateImageDescriptor(4, lut_rg_fallback.image, DkImageType_Buffer);
        UpdateSamplerDescriptor(4, sampler);
        handles[4] = dkMakeTextureHandle(4, 4);

        UpdateImageDescriptor(5, lut_rgba_fallback.image, DkImageType_Buffer);
        UpdateSamplerDescriptor(5, sampler);
        handles[5] = dkMakeTextureHandle(5, 5);

        white_texture.UploadIfNeeded(command_buffer);
        dk::Sampler white_sampler;
        white_sampler.setFilter(DkFilter_Linear, DkFilter_Linear);
        white_sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge,
                                  DkWrapMode_ClampToEdge);
        white_sampler.setLodClamp(0.0f, 0.0f);
        for (u32 slot : {6U, 7U}) {
            UpdateImageDescriptor(slot, white_texture.image);
            UpdateSamplerDescriptor(slot, white_sampler);
            handles[slot] = dkMakeTextureHandle(slot, slot);
        }
    }

    void PatchLutUniformOffsets(Pica::Shader::Generator::FSUniformData& fsu) const {
        for (u32 index = 0; index < Pica::LightingRegs::NumLightingSampler; ++index) {
            fsu.lighting_lut_offset[index / 4][index % 4] = static_cast<int>(index * 256);
        }
        fsu.fog_lut_offset = static_cast<int>(Pica::LightingRegs::NumLightingSampler * 256);
        fsu.proctex_noise_lut_offset = 0;
        fsu.proctex_color_map_offset = 128;
        fsu.proctex_alpha_map_offset = 256;
        fsu.proctex_lut_offset = 0;
        fsu.proctex_diff_lut_offset = 256;
    }

    bool SyncAndUploadPicaLUTs(Pica::PicaCore& pica) {
        std::vector<Common::Vec2f> lf_data(LutLfElementCount, {1.0f, 0.0f});
        for (u32 lut = 0; lut < Pica::LightingRegs::NumLightingSampler; ++lut) {
            const auto& source_lut = pica.lighting.luts[lut];
            Common::Vec2f* dst = lf_data.data() + lut * 256;
            for (u32 i = 0; i < source_lut.size(); ++i) {
                dst[i] = {source_lut[i].ToFloat(), source_lut[i].DiffToFloat()};
            }
        }
        Common::Vec2f* fog_dst =
            lf_data.data() + Pica::LightingRegs::NumLightingSampler * 256;
        for (u32 i = 0; i < pica.fog.lut.size(); ++i) {
            fog_dst[i] = {pica.fog.lut[i].ToFloat(), pica.fog.lut[i].DiffToFloat()};
        }

        std::vector<Common::Vec2f> rg_data(LutRgElementCount, {1.0f, 0.0f});
        const auto sync_value_lut = [&rg_data](u32 offset, const auto& source_lut) {
            Common::Vec2f* dst = rg_data.data() + offset;
            for (u32 i = 0; i < source_lut.size(); ++i) {
                dst[i] = {source_lut[i].ToFloat(), source_lut[i].DiffToFloat()};
            }
        };
        sync_value_lut(0, pica.proctex.noise_table);
        sync_value_lut(128, pica.proctex.color_map_table);
        sync_value_lut(256, pica.proctex.alpha_map_table);

        std::vector<Common::Vec4f> rgba_data(LutRgbaElementCount,
                                             {1.0f, 1.0f, 1.0f, 1.0f});
        for (u32 i = 0; i < pica.proctex.color_table.size(); ++i) {
            rgba_data[i] = pica.proctex.color_table[i].ToVector() / 255.0f;
        }
        for (u32 i = 0; i < pica.proctex.color_diff_table.size(); ++i) {
            rgba_data[256 + i] = pica.proctex.color_diff_table[i].ToVector() / 255.0f;
        }

        const bool uploaded =
            lut_lf_fallback.Upload(lf_data.data(),
                                   static_cast<u32>(lf_data.size() * sizeof(lf_data[0]))) &&
            lut_rg_fallback.Upload(rg_data.data(),
                                   static_cast<u32>(rg_data.size() * sizeof(rg_data[0]))) &&
            lut_rgba_fallback.Upload(rgba_data.data(),
                                     static_cast<u32>(rgba_data.size() * sizeof(rgba_data[0])));
        if (uploaded) {
            pica.lighting.lut_dirty = 0;
            pica.fog.lut_dirty = false;
            pica.proctex.table_dirty = 0;
        }
        return uploaded;
    }

    bool BindTexturesForBatch(const std::array<PresentTextureConfig, PicaTextureUnitCount>& textures,
                              RasterizerDeko3D& rasterizer) {
        std::array<DkResHandle, TextureDescriptorSlots> handles{};
        for (std::size_t slot = 0; slot < textures.size(); ++slot) {
            const TextureBinding binding = rasterizer.GetTextureBinding(textures[slot]);
            if (!binding.valid) {
#ifdef __SWITCH__
                const auto& texture = textures[slot];
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.stack step=texture-bind.invalid "
                    "func=Context::BindTexturesForBatch line=%d slot=%u enabled=%u "
                    "addr=%08X size=%ux%u format=%u",
                    __LINE__, static_cast<u32>(slot), texture.enabled ? 1 : 0,
                    TextureAddress(texture), TextureWidth(texture), TextureHeight(texture),
                    static_cast<u32>(texture.format));
#endif
                return false;
            }
            dk::ImageDescriptor image_descriptor;
            image_descriptor.initialize(binding.image_view);
            image_descriptor_set.Update(command_buffer, static_cast<u32>(slot), image_descriptor);
            UpdateSamplerDescriptor(static_cast<u32>(slot), binding.sampler);
            handles[slot] = dkMakeTextureHandle(static_cast<u32>(slot), static_cast<u32>(slot));
        }
        if (EnableLutBinding) {
            BindLutFallbackDescriptors(handles);
        }

        command_buffer.barrier(DkBarrier_None, DkInvalidateFlags_Image);
        command_buffer.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
        image_descriptor_set.BindForImages(command_buffer);
        sampler_descriptor_set.BindForSamplers(command_buffer);
        command_buffer.bindTextures(DkStage_Fragment, 0, handles);
        return true;
    }

    bool BindTextureForBatch(const PresentTextureConfig& texture, RasterizerDeko3D& rasterizer) {
        std::array<PresentTextureConfig, PicaTextureUnitCount> textures{};
        textures[0] = texture;
        return BindTexturesForBatch(textures, rasterizer);
    }

    void BindRenderTargetTexture(DekoRenderTarget& source) {
        dk::ImageView image_view{source.image};
        dk::ImageDescriptor image_descriptor;
        image_descriptor.initialize(image_view);
        image_descriptor_set.Update(command_buffer, 0, image_descriptor);

        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Linear, DkFilter_Linear);
        sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
        dk::SamplerDescriptor sampler_descriptor;
        sampler_descriptor.initialize(sampler);
        sampler_descriptor_set.Update(command_buffer, 0, sampler_descriptor);
        command_buffer.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
        image_descriptor_set.BindForImages(command_buffer);
        sampler_descriptor_set.BindForSamplers(command_buffer);
        command_buffer.bindTextures(DkStage_Fragment, 0, dkMakeTextureHandle(0, 0));
    }

    void DrawPresentVertices(const std::vector<PresentVertex>& vertices,
                             const Common::Rectangle<u32>& dst, u32 frame_count,
                             bool using_cached_vertices, float alpha_cap, bool blend_enabled,
                             bool rotate_clockwise, const char* pass_name,
                             const PresentBatch::RenderState* pica_render_state = nullptr,
                             bool force_trace = false) {
        const bool dst_valid = dst.right > dst.left && dst.bottom > dst.top;
        const auto drawable_vertices = BuildDrawableVertices(vertices, alpha_cap, rotate_clockwise);

#ifdef __SWITCH__
        const bool has_vertices = !vertices.empty();
        const bool is_pica_target_pass = std::strcmp(pass_name, "pica-target") == 0;
        const bool should_log_present =
            DekoHotTrace &&
            (force_trace ||
             (is_pica_target_pass
                  ? (present_draw_log_count < 8 || (present_draw_log_count % 2048) == 0)
                  : (present_draw_log_count < 16 || present_draw_log_count == 32 ||
                     (present_draw_log_count % 60) == 0 ||
                     (has_vertices && (present_nonempty_log_count < 32 ||
                                       (present_nonempty_log_count % 60) == 0)))));
        if (should_log_present) {
            u32 visible_vertices = 0;
            float min_x = std::numeric_limits<float>::max();
            float max_x = std::numeric_limits<float>::lowest();
            float min_y = std::numeric_limits<float>::max();
            float max_y = std::numeric_limits<float>::lowest();
            for (const auto& vertex : vertices) {
                const float w = vertex.position[3];
                if (w == 0.0f) {
                    continue;
                }
                const float x = vertex.position[0] / w;
                const float y = vertex.position[1] / w;
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
                if (x >= -1.0f && x <= 1.0f && y >= -1.0f && y <= 1.0f) {
                    ++visible_vertices;
                }
            }

            const PresentVertex first = vertices.empty() ? PresentVertex{} : vertices.front();
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.present-handoff pass=%s attempt=%u frame=%u count=%u "
                "drawable=%u cached=%u dst-valid=%u dst=%u,%u,%u,%u visible=%u "
                "ndc-x=%f..%f ndc-y=%f..%f first-pos=%f,%f,%f,%f "
                "first-color=%f,%f,%f,%f",
                pass_name, present_draw_log_count, frame_count, static_cast<u32>(vertices.size()),
                static_cast<u32>(drawable_vertices.size()), using_cached_vertices ? 1 : 0,
                dst_valid ? 1 : 0, dst.left, dst.top, dst.right, dst.bottom, visible_vertices, min_x,
                max_x, min_y, max_y, first.position[0], first.position[1], first.position[2],
                first.position[3], first.color[0], first.color[1], first.color[2], first.color[3]);
        }
        if (has_vertices) {
            ++present_nonempty_log_count;
        }
        ++present_draw_log_count;
#endif

        if (drawable_vertices.empty() || !dst_valid) {
#ifdef __SWITCH__
            if (should_log_present) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.present-draw.skip "
                    "func=DrawPresentVertices line=%u pass=%s frame=%u drawable=%u dst-valid=%u",
                    static_cast<u32>(__LINE__), pass_name, frame_count,
                    static_cast<u32>(drawable_vertices.size()), dst_valid ? 1 : 0);
            }
#endif
            return;
        }

        const u32 byte_size = static_cast<u32>(drawable_vertices.size() * sizeof(PresentVertex));
        const u32 vertex_offset =
            AlignUp(present_vertex_frame_offset, static_cast<u32>(alignof(PresentVertex)));
        const u32 required_size = vertex_offset + byte_size;
        if (required_size > present_vertex_capacity || !present_vertex_memory.block) {
            LOG_ERROR(Render, "Deko3D present vertex buffer exhausted required={} capacity={}",
                      required_size, present_vertex_capacity);
            return;
        }

        auto* vertex_dst =
            static_cast<u8*>(present_vertex_memory.block.getCpuAddr()) + vertex_offset;
        std::memcpy(vertex_dst, drawable_vertices.data(), byte_size);
        present_vertex_memory.block.flushCpuCache(vertex_offset, byte_size);
#ifdef __SWITCH__
        if (should_log_present) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.present-vbuf.ready "
                "func=DrawPresentVertices line=%u pass=%s frame=%u upload-count=%u "
                "byte-size=%u offset=%u required=%u",
                static_cast<u32>(__LINE__), pass_name, frame_count,
                static_cast<u32>(drawable_vertices.size()), byte_size, vertex_offset,
                required_size);
        }
#endif

        const bool use_pica_render_state =
            pica_render_state != nullptr && override_vertex_shader != nullptr;
        DkViewport viewport{static_cast<float>(dst.left),
                            static_cast<float>(dst.top),
                            static_cast<float>(dst.right - dst.left),
                            static_cast<float>(dst.bottom - dst.top),
                            0.0f,
                            1.0f};
        DkScissor scissor{dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top};
        if (use_pica_render_state && pica_render_state->viewport_valid) {
            viewport = DkViewport{static_cast<float>(pica_render_state->viewport_x),
                                  static_cast<float>(pica_render_state->viewport_y),
                                  static_cast<float>(pica_render_state->viewport_width),
                                  static_cast<float>(pica_render_state->viewport_height),
                                  0.0f,
                                  1.0f};
            scissor = DkScissor{pica_render_state->scissor_x, pica_render_state->scissor_y,
                                pica_render_state->scissor_width,
                                pica_render_state->scissor_height};
        }
        // If the caller (currently only DrawPicaColorTarget) installed an
        // override shader pair, use them instead of the present shaders.
        // The override is cleared after each pica-target draw so subsequent
        // present/blit passes keep using the offline present shaders.
        const dk::Shader* vs_to_bind =
            override_vertex_shader ? override_vertex_shader : &present_vertex_shader;
        const dk::Shader* fs_to_bind =
            override_fragment_shader ? override_fragment_shader : &present_fragment_shader;
        const std::array<DkShader const*, 2> shaders{vs_to_bind, fs_to_bind};
        // Bind only when the pair actually changed. Each bindShaders call
        // forces a Maxwell pipeline-state switch which is expensive; many
        // consecutive batches share the same FSConfig in practice, so this
        // collapses 300+ calls/frame down to ~tens.
        const bool shaders_changed =
            vs_to_bind != last_bound_vs || fs_to_bind != last_bound_fs;
#ifdef __SWITCH__
        if (should_log_present) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.present-state.begin "
                "func=DrawPresentVertices line=%u pass=%s frame=%u shaders-changed=%u "
                "use-pica-state=%u",
                static_cast<u32>(__LINE__), pass_name, frame_count, shaders_changed ? 1 : 0,
                pica_render_state != nullptr && override_vertex_shader != nullptr ? 1 : 0);
        }
#endif

        dk::RasterizerState rasterizer_state;
        dk::ColorState color_state;
        dk::ColorWriteState color_write_state;
        dk::BlendState blend_state;
        dk::DepthStencilState depth_stencil_state;
        if (use_pica_render_state) {
            const auto& rs = *pica_render_state;
            const auto cull_mode = static_cast<Pica::RasterizerRegs::CullMode>(rs.cull_mode);
            rasterizer_state
                .setCullMode(PicaToDeko::CullMode(cull_mode, rs.flip_viewport))
                .setFrontFace(PicaToDeko::FrontFace(cull_mode));
            color_state.setBlendEnable(0, rs.blend_enable);
            color_state.setLogicOp(PicaToDeko::LogicOp(
                static_cast<Pica::FramebufferRegs::LogicOp>(rs.logic_op)));
            color_write_state.setMask(0, rs.color_write_mask);
            blend_state.setOps(
                PicaToDeko::BlendEquation(
                    static_cast<Pica::FramebufferRegs::BlendEquation>(rs.blend_eq_rgb)),
                PicaToDeko::BlendEquation(
                    static_cast<Pica::FramebufferRegs::BlendEquation>(rs.blend_eq_a)));
            blend_state.setFactors(
                PicaToDeko::BlendFunc(
                    static_cast<Pica::FramebufferRegs::BlendFactor>(rs.src_factor_rgb)),
                PicaToDeko::BlendFunc(
                    static_cast<Pica::FramebufferRegs::BlendFactor>(rs.dst_factor_rgb)),
                PicaToDeko::BlendFunc(
                    static_cast<Pica::FramebufferRegs::BlendFactor>(rs.src_factor_a)),
                PicaToDeko::BlendFunc(
                    static_cast<Pica::FramebufferRegs::BlendFactor>(rs.dst_factor_a)));
            const Common::Vec4f blend_const = PicaToDeko::ColorRGBA8(rs.blend_const);
            command_buffer.setBlendConst(blend_const.x, blend_const.y, blend_const.z,
                                         blend_const.w);
        } else {
            color_state.setBlendEnable(0, blend_enabled);
        }
        // Stencil-state path temporarily disabled (v89 bisect). Ryujinx
        // crashes the same way in v87/v88; we keep depth on Z24S8 and the
        // capture in render_state lives so we can flip this back once we
        // identify which deko3d state call goes wrong. For now stencil
        // test stays off, mirroring v86's behavior.
        constexpr bool kStencilStateLive = false;
        if (use_pica_render_state && EnableDepthAttachment) {
            const auto& rs = *pica_render_state;
            depth_stencil_state
                .setDepthTestEnable(rs.depth_test_enable)
                .setDepthWriteEnable(rs.depth_write_enable)
                .setDepthCompareOp(PicaToDeko::CompareFunc(
                    static_cast<Pica::FramebufferRegs::CompareFunc>(rs.depth_test_func)))
                .setStencilTestEnable(kStencilStateLive && rs.stencil_enable);
            if (kStencilStateLive && rs.stencil_enable) {
                depth_stencil_state.setStencilFrontCompareOp(PicaToDeko::CompareFunc(
                    static_cast<Pica::FramebufferRegs::CompareFunc>(rs.stencil_test_func)));
                depth_stencil_state.setStencilFrontFailOp(PicaToDeko::StencilOp(
                    static_cast<Pica::FramebufferRegs::StencilAction>(rs.stencil_action_fail)));
                depth_stencil_state.setStencilFrontDepthFailOp(PicaToDeko::StencilOp(
                    static_cast<Pica::FramebufferRegs::StencilAction>(rs.stencil_action_depth_fail)));
                depth_stencil_state.setStencilFrontPassOp(PicaToDeko::StencilOp(
                    static_cast<Pica::FramebufferRegs::StencilAction>(rs.stencil_action_depth_pass)));
                // PICA tracks only one stencil face; mirror to back.
                depth_stencil_state.setStencilBackCompareOp(PicaToDeko::CompareFunc(
                    static_cast<Pica::FramebufferRegs::CompareFunc>(rs.stencil_test_func)));
                depth_stencil_state.setStencilBackFailOp(PicaToDeko::StencilOp(
                    static_cast<Pica::FramebufferRegs::StencilAction>(rs.stencil_action_fail)));
                depth_stencil_state.setStencilBackDepthFailOp(PicaToDeko::StencilOp(
                    static_cast<Pica::FramebufferRegs::StencilAction>(rs.stencil_action_depth_fail)));
                depth_stencil_state.setStencilBackPassOp(PicaToDeko::StencilOp(
                    static_cast<Pica::FramebufferRegs::StencilAction>(rs.stencil_action_depth_pass)));
                // deko3d signature: setStencil(face, writeMask, funcRef, funcMask).
                // We had write and input masks swapped in v87 — that likely
                // produced garbage write masks and crashed Ryujinx around
                // frame 265.
                command_buffer.setStencil(DkFace_FrontAndBack, rs.stencil_write_mask,
                                          rs.stencil_ref, rs.stencil_input_mask);
            }
        } else {
            depth_stencil_state.setDepthTestEnable(false).setDepthWriteEnable(false);
        }

        command_buffer.setViewports(0, {viewport});
        command_buffer.setScissors(0, {scissor});
        // Always re-bind. The dedupe attempt (skipping bindShaders when
        // pair unchanged) crashed the GPU after a few hundred batches —
        // bindShaders likely re-validates other pipeline state that we
        // can't safely skip without a wider audit. Track last_bound_* but
        // don't use them to gate the bind for now.
        command_buffer.bindShaders(DkStageFlag_GraphicsMask, shaders);
        last_bound_vs = vs_to_bind;
        last_bound_fs = fs_to_bind;
        (void)shaders_changed;
        command_buffer.bindRasterizerState(rasterizer_state);
        command_buffer.bindColorState(color_state);
        command_buffer.bindColorWriteState(color_write_state);
        command_buffer.bindBlendStates(0, {blend_state});
        command_buffer.bindDepthStencilState(depth_stencil_state);
        command_buffer.bindVtxBuffer(0, present_vertex_memory.block.getGpuAddr() + vertex_offset,
                                     byte_size);
        // PICA-target passes use the generated trivial VS, which declares all
        // post-geometry PICA outputs as vertex inputs.
        const bool use_pica_vtx_state = override_vertex_shader != nullptr;
        if (use_pica_vtx_state) {
            command_buffer.bindVtxAttribState(PicaVertexAttribState);
            command_buffer.bindVtxBufferState(PicaVertexBufferState);
        } else {
            command_buffer.bindVtxAttribState(PresentVertexAttribState);
            command_buffer.bindVtxBufferState(PresentVertexBufferState);
        }
#ifdef __SWITCH__
        if (is_pica_target_pass &&
            (pica_vtx_bind_log_count < 4 || ShouldTraceDekoFrameSummary(frame_count))) {
            const auto* rs = pica_render_state;
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.pica-vtx-bind attempt=%u use-pica=%u "
                "override-vs=%u viewport=%d,%d,%d,%d scissor=%u,%u,%u,%u "
                "cull=%u flip=%u first-w=%f",
                pica_vtx_bind_log_count, use_pica_vtx_state ? 1 : 0,
                override_vertex_shader ? 1 : 0, rs != nullptr ? rs->viewport_x : 0,
                rs != nullptr ? rs->viewport_y : 0, rs != nullptr ? rs->viewport_width : 0,
                rs != nullptr ? rs->viewport_height : 0, rs != nullptr ? rs->scissor_x : 0,
                rs != nullptr ? rs->scissor_y : 0, rs != nullptr ? rs->scissor_width : 0,
                rs != nullptr ? rs->scissor_height : 0, rs != nullptr ? rs->cull_mode : 0,
                rs != nullptr && rs->flip_viewport ? 1 : 0,
                drawable_vertices.empty() ? 0.0f : drawable_vertices.front().position[3]);
            if (pica_vtx_bind_log_count < 4) {
                ++pica_vtx_bind_log_count;
            }
        }
#endif
#ifdef __SWITCH__
        if (should_log_present) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.present-draw.begin "
                "func=DrawPresentVertices line=%u pass=%s frame=%u upload-count=%u "
                "byte-size=%u offset=%u use-pica-vtx=%u dst=%u,%u,%u,%u",
                static_cast<u32>(__LINE__), pass_name, frame_count,
                static_cast<u32>(drawable_vertices.size()), byte_size, vertex_offset,
                use_pica_vtx_state ? 1 : 0, dst.left, dst.top, dst.right, dst.bottom);
        }
#endif
        command_buffer.draw(DkPrimitive_Triangles, static_cast<u32>(drawable_vertices.size()), 1, 0,
                            0);
#ifdef __SWITCH__
        if (should_log_present) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.present-draw.end "
                "func=DrawPresentVertices line=%u pass=%s frame=%u upload-count=%u",
                static_cast<u32>(__LINE__), pass_name, frame_count,
                static_cast<u32>(drawable_vertices.size()));
        }
#endif
        present_vertex_frame_offset = required_size;

#ifdef __SWITCH__
        if (should_log_present || (!is_pica_target_pass && ShouldTraceDekoFrame(frame_count))) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.present-vertices pass=%s frame=%u count=%u "
                "upload-count=%u cached=%u alpha=%f blend=%u dst=%u,%u,%u,%u",
                pass_name, frame_count, static_cast<u32>(vertices.size()),
                static_cast<u32>(drawable_vertices.size()), using_cached_vertices ? 1 : 0,
                alpha_cap, blend_enabled ? 1 : 0, dst.left, dst.top, dst.right, dst.bottom);
        }
#endif
    }

    bool DrawPicaColorTarget(const std::vector<PresentBatch>& batches, const DekoScreenFrame& top,
                             u32 frame_count, bool using_cached_batches,
                             RasterizerDeko3D& rasterizer,
                             Pica::PicaCore& pica, const DekoPicaTargetInfo& pica_target,
                             const Pica::RegsInternal& live_regs,
                             const Pica::Shader::Generator::FSUniformData& fsu,
                             const Pica::Shader::Generator::VSUniformData& vsu,
                             bool clear_target = true) {
        if (!pica_target.enabled || batches.empty() || !top.enabled || top.width == 0 ||
            top.height == 0) {
#ifdef __SWITCH__
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.pica-target.return-false "
                "func=DrawPicaColorTarget line=%u reason=not-ready "
                "frame=%u target-enabled=%u batches=%u top-enabled=%u top-size=%ux%u",
                static_cast<u32>(__LINE__), frame_count, pica_target.enabled ? 1U : 0U,
                static_cast<u32>(batches.size()), top.enabled ? 1U : 0U, top.width,
                top.height);
#endif
            return false;
        }

#ifdef __SWITCH__
        // Increment 4 smoke test: at the first frame with PICA batches, build a
        // PICA fragment shader config from live regs, generate GLSL, and try to
        // compile it through uam. Read-only — does not bind the result yet.
        // Validates that GenerateFragmentShader produces something uam accepts.
        static std::atomic_bool fs_gen_smoke_done{false};
        bool fs_expected = false;
        if (fs_gen_smoke_done.compare_exchange_strong(fs_expected, true)) {
            Pica::Shader::FSConfig fs_config{live_regs};
            Pica::Shader::UserConfig user_config{};
            Pica::Shader::Profile profile{};
            profile.has_separable_shaders = 1;
            profile.has_clip_planes = 1;
            profile.has_geometry_shader = 1;
            profile.has_custom_border_color = 1;
            profile.has_logic_op = 1;
            // deko3d uses Vulkan-style [0,1] depth range; with =1 the FS would
        // assume OpenGL [-1,1] and compute gl_FragDepth values outside the
        // valid Vulkan range. Result: depth values clamp/collapse and the
        // depth test rejects overlay batches.
        profile.has_minus_one_to_one_range = 0;
            profile.is_vulkan = 0;

            std::string glsl =
                Pica::Shader::Generator::GLSL::GenerateFragmentShader(fs_config, user_config,
                                                                       profile);
            // GenerateFragmentShader omits the #version directive (the OpenGL
            // renderer prepends "#version 430 core" before glCompileShader).
            // uam requires #version too — match the present-shader version
            // we already know works.
            glsl.insert(0, "#version 460 core\n");

            void* dksh = nullptr;
            size_t dksh_size = 0;
            const int rc = uam_compile_glsl(UAM_STAGE_FRAGMENT, glsl.c_str(), &dksh, &dksh_size);
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.fs-gen.smoke frame=%u glsl-size=%zu rc=%d dksh-size=%zu",
                frame_count, glsl.size(), rc, dksh_size);

            // When compilation fails, dump the first 800 chars of the GLSL so
            // we can see version directive + extension list + the start of the
            // shader. Most uam-vs-PICA mismatches surface in the preamble.
            if (rc != 0) {
                const std::size_t to_dump = std::min<std::size_t>(glsl.size(), 800);
                std::string preview(glsl, 0, to_dump);
                // Replace newlines with " | " so we get one log line.
                for (auto& c : preview) {
                    if (c == '\n' || c == '\r') c = '|';
                }
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.fs-gen.dump frame=%u preview=\"%s\"",
                    frame_count, preview.c_str());
            }
            if (dksh) {
                uam_free(dksh);
            }

            // Now compile the matching trivial vertex shader — passthrough
            // that forwards CPU-shaded vertex outputs to the FS we just
            // generated. Same #version prepend trick.
            std::string vs_glsl =
                Pica::Shader::Generator::GLSL::GenerateTrivialVertexShader(
                    /*use_clip_planes=*/false, /*separable_shader=*/true);
            vs_glsl.insert(0, "#version 460 core\n");
            void* vs_dksh = nullptr;
            size_t vs_dksh_size = 0;
            const int vs_rc =
                uam_compile_glsl(UAM_STAGE_VERTEX, vs_glsl.c_str(), &vs_dksh, &vs_dksh_size);
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.vs-gen.smoke frame=%u glsl-size=%zu rc=%d dksh-size=%zu",
                frame_count, vs_glsl.size(), vs_rc, vs_dksh_size);
            if (vs_rc != 0) {
                const std::size_t to_dump = std::min<std::size_t>(vs_glsl.size(), 800);
                std::string preview(vs_glsl, 0, to_dump);
                for (auto& c : preview) {
                    if (c == '\n' || c == '\r') c = '|';
                }
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.vs-gen.dump frame=%u preview=\"%s\"",
                    frame_count, preview.c_str());
            }
            if (vs_dksh) {
                uam_free(vs_dksh);
            }

            // Step 5b: copy the live FS/VS uniform data into the GPU uniform
            // memblock. flushCpuCache after each region so the device sees
            // fresh contents on the next draw. Still not bound — that's the
            // next iteration.
            u32 ubo_write_status = 0;
            if (pica_uniform_memory.block) {
                auto* base = static_cast<u8*>(pica_uniform_memory.block.getCpuAddr());
                std::memcpy(base + pica_uniform_vs_offset, &vsu, sizeof(vsu));
                std::memcpy(base + pica_uniform_fs_offset, &fsu, sizeof(fsu));
                pica_uniform_memory.block.flushCpuCache(
                    pica_uniform_vs_offset,
                    pica_uniform_fs_offset + sizeof(fsu) - pica_uniform_vs_offset);
                ubo_write_status = 1;
            }
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.uniforms.write fs-size=%zu vs-size=%zu "
                "vs-off=%u fs-off=%u write=%u "
                "depth-scale=%f alpha-ref=%d",
                sizeof(Pica::Shader::Generator::FSUniformData),
                sizeof(Pica::Shader::Generator::VSUniformData),
                pica_uniform_vs_offset, pica_uniform_fs_offset, ubo_write_status,
                fsu.depth_scale, fsu.alphatest_ref);
        }
#endif

        auto [color_it, color_inserted] = pica_color_targets.try_emplace(pica_target.color_address);
        DekoRenderTarget& color_target = color_it->second;
        const bool changed = !color_target.ready ||
                             color_target.address != pica_target.color_address ||
                             color_target.width != pica_target.width ||
                             color_target.height != pica_target.height ||
                             color_target.color_format !=
                                 DekoRenderTarget::MapColorFormat(pica_target.color_format);
        if (!color_target.Ensure(device, queue, pica_target.color_address, pica_target.width,
                                 pica_target.height, pica_target.color_format)) {
            LOG_ERROR(Render, "Deko3D PICA color target creation failed");
#ifdef __SWITCH__
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.pica-target.return-false "
                "func=DrawPicaColorTarget line=%u reason=color-target-ensure "
                "frame=%u address=%08X size=%ux%u format=%u",
                static_cast<u32>(__LINE__), frame_count, pica_target.color_address,
                pica_target.width, pica_target.height,
                static_cast<u32>(pica_target.color_format));
#endif
            if (color_inserted) {
                pica_color_targets.erase(color_it);
            }
            return false;
        }
        // Paired depth attachment (when EnableDepthAttachment is on).
        bool depth_ready = false;
        DekoRenderTarget* depth_target = nullptr;
        if (EnableDepthAttachment) {
            auto [depth_it, depth_inserted] =
                pica_depth_targets.try_emplace(pica_target.color_address);
            depth_target = &depth_it->second;
            depth_ready = depth_target->EnsureDepth(device, queue, pica_target.color_address,
                                                    pica_target.width, pica_target.height);
            if (!depth_ready) {
                LOG_ERROR(Render, "Deko3D PICA depth target creation failed");
#ifdef __SWITCH__
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.pica-target.return-false "
                    "func=DrawPicaColorTarget line=%u reason=depth-target-ensure "
                    "frame=%u address=%08X size=%ux%u",
                    static_cast<u32>(__LINE__), frame_count, pica_target.color_address,
                    pica_target.width, pica_target.height);
#endif
                if (depth_inserted) {
                    pica_depth_targets.erase(depth_it);
                }
                return false;
            }
        }

        dk::ImageView target_view{color_target.image};
        const std::array<DkImageView const*, 1> render_targets{&target_view};
        if (depth_ready && depth_target != nullptr) {
            dk::ImageView depth_view{depth_target->image};
            command_buffer.bindRenderTargets(render_targets, &depth_view);
        } else {
            command_buffer.bindRenderTargets(render_targets);
        }
        if (clear_target) {
            command_buffer.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
            if (depth_ready) {
                command_buffer.clearDepthStencil(true, 1.0f, 0xFF, 0);
            }
        }

        const u32 lighting_dirty_before = pica.lighting.lut_dirty;
        const u32 proctex_dirty_before = pica.proctex.table_dirty;
        const bool fog_dirty_before = pica.fog.lut_dirty;
        const bool uploaded_luts = EnableLutBinding && SyncAndUploadPicaLUTs(pica);
        if (uploaded_luts) {
            command_buffer.barrier(DkBarrier_None, DkInvalidateFlags_Image);
        }
#ifdef __SWITCH__
        if (lut_upload_log_count < 8) {
            ++lut_upload_log_count;
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.lut-upload frame=%u uploaded=%u "
                "lighting-dirty=%08X fog-dirty=%u proctex-dirty=%02X "
                "lf-elements=%u rg-elements=%u rgba-elements=%u",
                frame_count, uploaded_luts ? 1U : 0U, lighting_dirty_before,
                fog_dirty_before ? 1U : 0U, proctex_dirty_before, LutLfElementCount,
                LutRgElementCount, LutRgbaElementCount);
        }
#endif

        // --- Experimental runtime PICA shader bind ---
        // When enabled, compile (or reuse) the PICA FS+VS pair for this
        // frame's reg state, write the per-frame UBOs, and override the
        // present-shader bind that DrawPresentVertices does below. If
        // anything in the chain fails (compile, layout), we drop back to
        // the present shaders for this frame — never crash the renderer.
        bool runtime_pica_active = false;
        if (ExperimentalRuntimePicaShaders &&
            EnsurePicaRuntimeShaders(live_regs) &&
            pica_uniform_memory.block) {
            // Refresh UBO contents from the freshly-synced uniforms.
            // RasterizerAccelerated::SyncDrawUniforms doesn't populate scissor
            // — that's normally done by the per-API rasterizer in its draw
            // path (gl_rasterizer / vk_rasterizer). We have no equivalent
            // step in the deko3d path yet, so the scissor fields stay zero
            // and the FS discards every fragment. Patch the live PICA
            // scissor reg values into a local copy before writing.
            Pica::Shader::Generator::FSUniformData fsu_patched = fsu;
            const auto& sc = live_regs.rasterizer.scissor_test;
            fsu_patched.scissor_x1 = static_cast<int>(sc.x1.Value());
            fsu_patched.scissor_y1 = static_cast<int>(sc.y1.Value());
            fsu_patched.scissor_x2 = static_cast<int>(sc.x2.Value());
            fsu_patched.scissor_y2 = static_cast<int>(sc.y2.Value());
            // DrawTriangles now mirrors the Vulkan/OpenGL software-VS path:
            // upload clip-space positions unchanged and let the trivial VS
            // plus dynamic viewport handle framebuffer flips.
            Pica::Shader::Generator::VSUniformData vsu_patched = vsu;
            PatchLutUniformOffsets(fsu_patched);
            auto* base = static_cast<u8*>(pica_uniform_memory.block.getCpuAddr());
            std::memcpy(base + pica_uniform_vs_offset, &vsu_patched, sizeof(vsu_patched));
            std::memcpy(base + pica_uniform_fs_offset, &fsu_patched, sizeof(fsu_patched));
            pica_uniform_memory.block.flushCpuCache(
                pica_uniform_vs_offset,
                pica_uniform_fs_offset + sizeof(fsu_patched) - pica_uniform_vs_offset);

            // Bind the UBOs at the indices the generated GLSL expects:
            // vs_data → binding 1, fs_data → binding 2.
            const DkBufExtents vs_ubo{
                pica_uniform_memory.block.getGpuAddr() + pica_uniform_vs_offset,
                pica_uniform_vs_size};
            const DkBufExtents fs_ubo{
                pica_uniform_memory.block.getGpuAddr() + pica_uniform_fs_offset,
                pica_uniform_fs_size};
            command_buffer.bindUniformBuffer(DkStage_Vertex, 1, vs_ubo.addr, vs_ubo.size);
            command_buffer.bindUniformBuffer(DkStage_Fragment, 2, fs_ubo.addr, fs_ubo.size);

            // Install the override so DrawPresentVertices picks up the
            // runtime-compiled shaders for this batch loop.
            override_vertex_shader = &pica_runtime_vs;
            override_fragment_shader = &pica_runtime_fs;
            runtime_pica_active = true;
        }

        const Common::Rectangle<u32> target_rect{0, 0, color_target.width, color_target.height};
        u32 vertex_count = 0;
        u32 textured_batches = 0;
        u32 bound_texture_units = 0;
        u32 skipped_texture_batches = 0;
        u32 drawn_batches = 0;
        u32 cache_lookups_this_frame = 0;
        u32 cache_misses_this_frame = 0;
        u32 compile_failures_this_frame = 0;
        u32 compile_cap_fallbacks_this_frame = 0;
        if (pica_compile_frame != frame_count) {
            pica_compile_frame = frame_count;
            pica_compiles_this_frame = 0;
        }
        u32 batch_index = 0;
        for (const auto& batch : batches) {
            const u32 current_batch = batch_index++;
#ifdef __SWITCH__
            const bool trace_batch_stack = ShouldTraceBatchStack(batch);
            if (trace_batch_stack) {
                LogBatchStack("batch.begin", frame_count, current_batch, batch, __LINE__);
            }
#endif
            vertex_count += static_cast<u32>(batch.vertices.size());
            const u32 enabled_texture_units = static_cast<u32>(std::count_if(
                batch.texture_configs.begin(), batch.texture_configs.end(),
                [](const PresentTextureConfig& texture) { return texture.enabled; }));
            if (enabled_texture_units != 0) {
                ++textured_batches;
            }
            if (!BindTexturesForBatch(batch.texture_configs, rasterizer)) {
                ++skipped_texture_batches;
#ifdef __SWITCH__
                if (trace_batch_stack) {
                    LogBatchStack("batch.bind-skip", frame_count, current_batch, batch, __LINE__);
                }
#endif
                continue;
            }
#ifdef __SWITCH__
            if (trace_batch_stack) {
                LogBatchStack("batch.bound", frame_count, current_batch, batch, __LINE__);
            }
#endif
            bound_texture_units += enabled_texture_units;

            // Per-batch UBO update: each batch carries its own snapshot of
            // PICA TEV / blend / lod / border / lighting state. Write the
            // pair into the batch's own slot in the uniform ring and bind
            // it. Without this, all batches share whatever state the last
            // batch in the frame happened to leave behind, which produces
            // white / wrong colors whenever TEV const_color etc. changed
            // mid-frame (very common in Mario 3D Land's UI).
            if (runtime_pica_active && drawn_batches < MaxUniformSlots &&
                pica_uniform_memory.block && pica_uniform_slot_stride > 0) {
                const u32 slot_base = drawn_batches * pica_uniform_slot_stride;
                const u32 batch_vs_off = slot_base;
                const u32 batch_fs_off =
                    slot_base +
                    AlignUp(pica_uniform_vs_size, DK_UNIFORM_BUF_ALIGNMENT);
                auto* base = static_cast<u8*>(pica_uniform_memory.block.getCpuAddr());
                Pica::Shader::Generator::VSUniformData vsu_batch = batch.vs_uniform_data;
                Pica::Shader::Generator::FSUniformData fsu_batch = batch.fs_uniform_data;
                PatchLutUniformOffsets(fsu_batch);
                std::memcpy(base + batch_vs_off, &vsu_batch, sizeof(vsu_batch));
                std::memcpy(base + batch_fs_off, &fsu_batch, sizeof(fsu_batch));
                pica_uniform_memory.block.flushCpuCache(
                    batch_vs_off,
                    batch_fs_off + sizeof(fsu_batch) - batch_vs_off);
#ifdef __SWITCH__
                // One-shot dump of the first batch's FS uniform values so we
                // can hand-trace the TEV chain against the generated GLSL.
                // If the chain depends on const_color[N] or blend_color and
                // those land at (1,1,1,1), TEV multiplications collapse to
                // tex0 directly (good) or to white when masked.
                static std::atomic_bool fsu_values_dumped{false};
                bool fsu_expected = false;
                if (fsu_values_dumped.compare_exchange_strong(fsu_expected, true)) {
                    Azahar::Switch::AppendLogFormat(
                        nullptr,
                        "android-flow stage=deko3d.fsu.values "
                        "alpha-ref=%d depth-scale=%f depth-offset=%f "
                        "fb-scale=%d "
                        "const-0=%f,%f,%f,%f const-1=%f,%f,%f,%f "
                        "const-2=%f,%f,%f,%f const-3=%f,%f,%f,%f "
                        "const-4=%f,%f,%f,%f const-5=%f,%f,%f,%f "
                        "buf-color=%f,%f,%f,%f blend=%f,%f,%f,%f "
                        "lod-bias=%f,%f,%f border-0=%f,%f,%f,%f",
                        fsu_batch.alphatest_ref, fsu_batch.depth_scale,
                        fsu_batch.depth_offset, fsu_batch.framebuffer_scale,
                        fsu_batch.const_color[0].x, fsu_batch.const_color[0].y,
                        fsu_batch.const_color[0].z, fsu_batch.const_color[0].w,
                        fsu_batch.const_color[1].x, fsu_batch.const_color[1].y,
                        fsu_batch.const_color[1].z, fsu_batch.const_color[1].w,
                        fsu_batch.const_color[2].x, fsu_batch.const_color[2].y,
                        fsu_batch.const_color[2].z, fsu_batch.const_color[2].w,
                        fsu_batch.const_color[3].x, fsu_batch.const_color[3].y,
                        fsu_batch.const_color[3].z, fsu_batch.const_color[3].w,
                        fsu_batch.const_color[4].x, fsu_batch.const_color[4].y,
                        fsu_batch.const_color[4].z, fsu_batch.const_color[4].w,
                        fsu_batch.const_color[5].x, fsu_batch.const_color[5].y,
                        fsu_batch.const_color[5].z, fsu_batch.const_color[5].w,
                        fsu_batch.tev_combiner_buffer_color.x,
                        fsu_batch.tev_combiner_buffer_color.y,
                        fsu_batch.tev_combiner_buffer_color.z,
                        fsu_batch.tev_combiner_buffer_color.w,
                        fsu_batch.blend_color.x, fsu_batch.blend_color.y,
                        fsu_batch.blend_color.z, fsu_batch.blend_color.w,
                        fsu_batch.tex_lod_bias.x, fsu_batch.tex_lod_bias.y,
                        fsu_batch.tex_lod_bias.z,
                        fsu_batch.tex_border_color[0].x,
                        fsu_batch.tex_border_color[0].y,
                        fsu_batch.tex_border_color[0].z,
                        fsu_batch.tex_border_color[0].w);
                }
#endif
                command_buffer.bindUniformBuffer(
                    DkStage_Vertex, 1,
                    pica_uniform_memory.block.getGpuAddr() + batch_vs_off,
                    pica_uniform_vs_size);
                command_buffer.bindUniformBuffer(
                    DkStage_Fragment, 2,
                    pica_uniform_memory.block.getGpuAddr() + batch_fs_off,
                    pica_uniform_fs_size);
            }

            // Per-batch FS lookup: each batch may use a different PICA TEV
            // / alpha-test / blend configuration. Bind the cached runtime
            // FS for this batch's FSConfig. Falls back to the per-frame
            // shader (or the present fallback) if the cache miss can't
            // compile or the chunked PICA shader arena is exhausted.
            if (runtime_pica_active && batch.fs_config) {
                // UAM compilation can spike hard on real hardware when a new
                // scene introduces many PICA FS variants at once. Keep the
                // frame moving and let the cache warm over several presents.
                constexpr u32 MaxCompilesPerFrame = 8;
                auto it = pica_shader_cache.find(batch.fs_config_hash);
                if (it != pica_shader_cache.end()) {
                    override_fragment_shader = &it->second.fs;
                } else if (pica_compiles_this_frame < MaxCompilesPerFrame) {
                    ++cache_misses_this_frame;
#ifdef __SWITCH__
                    static std::atomic_uint compile_begin_log_count{0};
                    if (compile_begin_log_count.fetch_add(1, std::memory_order_relaxed) < 128 ||
                        ShouldTraceDekoFrameSummary(frame_count)) {
                        Azahar::Switch::AppendLogFormat(
                            nullptr,
                            "android-flow stage=deko3d.pica-shader-compile.begin "
                            "func=DrawPicaColorTarget line=%u frame=%u batch=%u "
                            "hash=%016llX cache-size=%zu compiles=%u cap=%u",
                            static_cast<u32>(__LINE__), frame_count, current_batch,
                            static_cast<unsigned long long>(batch.fs_config_hash),
                            pica_shader_cache.size(), pica_compiles_this_frame,
                            MaxCompilesPerFrame);
                    }
#endif
                    if (auto* cached =
                            GetOrCompilePicaFS(*batch.fs_config, batch.fs_config_hash)) {
                        override_fragment_shader = &cached->fs;
                    } else {
                        override_fragment_shader = &pica_runtime_fs;
                        ++compile_failures_this_frame;
#ifdef __SWITCH__
                        static std::atomic_uint compile_fallback_log_count{0};
                        if (compile_fallback_log_count.fetch_add(1, std::memory_order_relaxed) <
                            64) {
                            Azahar::Switch::AppendLogFormat(
                                nullptr,
                                "android-flow stage=deko3d.pica-shader-cache.fallback "
                                "func=DrawPicaColorTarget line=%u reason=compile-failed "
                                "frame=%u batch=%u hash=%016llX cache-size=%zu compiles=%u",
                                static_cast<u32>(__LINE__), frame_count, current_batch,
                                static_cast<unsigned long long>(batch.fs_config_hash),
                                pica_shader_cache.size(), pica_compiles_this_frame);
                        }
#endif
                    }
                    ++pica_compiles_this_frame;
                } else {
                    override_fragment_shader = &pica_runtime_fs;
                    ++cache_misses_this_frame;
                    ++compile_cap_fallbacks_this_frame;
#ifdef __SWITCH__
                    static std::atomic_uint cap_fallback_log_count{0};
                    if (cap_fallback_log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
                        Azahar::Switch::AppendLogFormat(
                            nullptr,
                            "android-flow stage=deko3d.pica-shader-cache.fallback "
                            "func=DrawPicaColorTarget line=%u reason=compile-cap "
                            "frame=%u batch=%u hash=%016llX cache-size=%zu compiles=%u cap=%u",
                            static_cast<u32>(__LINE__), frame_count, current_batch,
                            static_cast<unsigned long long>(batch.fs_config_hash),
                            pica_shader_cache.size(), pica_compiles_this_frame,
                            MaxCompilesPerFrame);
                    }
#endif
                }
                ++cache_lookups_this_frame;
            }

#ifdef __SWITCH__
            const bool force_draw_trace = trace_batch_stack;
            if (trace_batch_stack) {
                LogBatchStack("batch.draw-call.begin", frame_count, current_batch, batch, __LINE__);
            }
#else
            constexpr bool force_draw_trace = false;
#endif
            DrawPresentVertices(batch.vertices, target_rect, frame_count, using_cached_batches, 1.0f,
                                false, false, "pica-target", &batch.render_state,
                                force_draw_trace);
#ifdef __SWITCH__
            if (trace_batch_stack) {
                LogBatchStack("batch.draw-call.end", frame_count, current_batch, batch, __LINE__);
                LogBatchStack("batch.drawn", frame_count, current_batch, batch, __LINE__);
            }
#endif
            ++drawn_batches;
        }
        command_buffer.barrier(DkBarrier_Tiles, DkInvalidateFlags_Image);

        // Clear overrides so subsequent passes (BlitRenderTarget present, etc.)
        // go back to the offline present shaders.
        if (runtime_pica_active) {
            override_vertex_shader = nullptr;
            override_fragment_shader = nullptr;
#ifdef __SWITCH__
            if (ShouldTraceDekoFrameSummary(frame_count)) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.runtime-pica.active frame=%u drawn=%u "
                    "lookups=%u misses=%u compiles=%u failures=%u cap-fallbacks=%u "
                    "cache-size=%zu",
                    frame_count, drawn_batches, cache_lookups_this_frame,
                    cache_misses_this_frame, pica_compiles_this_frame,
                    compile_failures_this_frame, compile_cap_fallbacks_this_frame,
                    pica_shader_cache.size());
            }
#endif
        }

#ifdef __SWITCH__
        if (changed || target_render_log_count < 16 || ShouldTraceDekoFrameSummary(frame_count) ||
            (target_render_log_count % 120) == 0) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.target-render frame=%u changed=%u pica=%08X "
                "pica-size=%ux%u image=%ux%u batches=%u drawn=%u textured=%u skipped=%u "
                "bound-textures=%u vertices=%u cached=%u targets=%zu",
                frame_count, changed ? 1 : 0, static_cast<u32>(pica_target.color_address),
                pica_target.width, pica_target.height, color_target.width,
                color_target.height, static_cast<u32>(batches.size()), drawn_batches,
                textured_batches, skipped_texture_batches, bound_texture_units, vertex_count,
                using_cached_batches ? 1 : 0, pica_color_targets.size());
        }
        ++target_render_log_count;
#endif

        return drawn_batches != 0;
    }

    void BlitSource(DekoSourceImage& source, const DekoScreenFrame& screen,
                    const dk::ImageView& framebuffer_view) {
        if (!screen.enabled || !screen.has_rgb || screen.dst.right <= screen.dst.left ||
            screen.dst.bottom <= screen.dst.top) {
            return;
        }

        const DkCopyBuf upload_buffer{source.staging_memory.block.getGpuAddr(), 0, 0};
        const DkImageRect source_rect{0, 0, 0, source.width, source.height, 1};
        dk::ImageView source_view{source.image};
        command_buffer.copyBufferToImage(upload_buffer, source_view, source_rect);

        const DkImageRect dst_rect{screen.dst.left,
                                   screen.dst.top,
                                   0,
                                   screen.dst.right - screen.dst.left,
                                   screen.dst.bottom - screen.dst.top,
                                   1};
        command_buffer.blitImage(source_view, source_rect, framebuffer_view, dst_rect,
                                 DkBlitFlag_FilterNearest | DkBlitFlag_ModeBlit);
    }

    void BlitRenderTarget(DekoRenderTarget& source, const DekoScreenFrame& screen,
                          const dk::ImageView& framebuffer_view, u32 frame_count,
                          u32 source_left = 0,
                          u32 source_top = 0, u32 source_width = 0, u32 source_height = 0,
                          const char* pass_name = "top") {
        if (!source.ready || !screen.enabled || screen.dst.right <= screen.dst.left ||
            screen.dst.bottom <= screen.dst.top) {
            return;
        }

        if (source_width == 0) {
            source_width = source.width;
        }
        if (source_height == 0) {
            source_height = source.height;
        }
        source_left = std::min(source_left, source.width - 1);
        source_top = std::min(source_top, source.height - 1);
        source_width = std::min(source_width, source.width - source_left);
        source_height = std::min(source_height, source.height - source_top);

        const DkImageRect source_rect{source_left, source_top, 0, source_width, source_height, 1};
        dk::ImageView source_view{source.image};
        const DkImageRect dst_rect{screen.dst.left,
                                   screen.dst.top,
                                   0,
                                   screen.dst.right - screen.dst.left,
                                   screen.dst.bottom - screen.dst.top,
                                   1};
        command_buffer.blitImage(source_view, source_rect, framebuffer_view, dst_rect,
                                 DkBlitFlag_FilterNearest | DkBlitFlag_ModeBlit);

#ifdef __SWITCH__
        if (ShouldTraceDekoFrame(frame_count)) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.target-blit pass=%s frame=%u src=%u,%u,%u,%u "
                "dst=%u,%u,%u,%u",
                pass_name, frame_count, source_rect.x, source_rect.y, source_rect.width,
                source_rect.height, dst_rect.x, dst_rect.y, dst_rect.width, dst_rect.height);
        }
#endif
    }

    void DrawRenderTargetToScreen(DekoRenderTarget& source, const DekoScreenFrame& screen,
                                  u32 frame_count, u32 source_left = 0, u32 source_top = 0,
                                  u32 source_width = 0, u32 source_height = 0,
                                  const char* pass_name = "top") {
        if (!source.ready || !screen.enabled || screen.dst.right <= screen.dst.left ||
            screen.dst.bottom <= screen.dst.top) {
            return;
        }

        if (source_width == 0) {
            source_width = source.width;
        }
        if (source_height == 0) {
            source_height = source.height;
        }
        source_left = std::min(source_left, source.width - 1);
        source_top = std::min(source_top, source.height - 1);
        source_width = std::min(source_width, source.width - source_left);
        source_height = std::min(source_height, source.height - source_top);

        const float u0 = static_cast<float>(source_left) / static_cast<float>(source.width);
        const float u1 =
            static_cast<float>(source_left + source_width) / static_cast<float>(source.width);
        const float v0 = static_cast<float>(source_top) / static_cast<float>(source.height);
        const float v1 =
            static_cast<float>(source_top + source_height) / static_cast<float>(source.height);

        BindRenderTargetTexture(source);

        const PresentVertex top_left{{-1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
                                     {u1, v0}};
        const PresentVertex top_right{{1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
                                      {u1, v1}};
        const PresentVertex bottom_left{{-1.0f, -1.0f, 0.0f, 1.0f},
                                        {1.0f, 1.0f, 1.0f, 1.0f}, {u0, v0}};
        const PresentVertex bottom_right{{1.0f, -1.0f, 0.0f, 1.0f},
                                         {1.0f, 1.0f, 1.0f, 1.0f}, {u0, v1}};
        const std::vector<PresentVertex> vertices{
            top_left, bottom_left, top_right, top_right, bottom_left, bottom_right,
        };
        DrawPresentVertices(vertices, screen.dst, frame_count, false, 1.0f, false, false,
                            pass_name);

#ifdef __SWITCH__
        if (ShouldTraceDekoFrame(frame_count) || ShouldTraceDekoFrameSummary(frame_count)) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.target-draw pass=%s frame=%u src=%u,%u,%u,%u "
                "uv=%f,%f,%f,%f dst=%u,%u,%u,%u",
                pass_name, frame_count, source_left, source_top, source_width, source_height, u0,
                v0, u1, v1, screen.dst.left, screen.dst.top, screen.dst.right, screen.dst.bottom);
        }
#endif
    }

    static bool TransferMatchesScreen(const DisplayTransferRecord& transfer,
                                      const DekoScreenFrame& screen) {
        return screen.enabled && transfer.output_width == screen.height &&
               transfer.output_height == screen.width;
    }

    static bool DisplayTargetMatchesScreen(const DekoRenderTarget* target,
                                           const DekoScreenFrame& screen) {
        return target != nullptr && target->ready && screen.enabled &&
               target->width == screen.height && target->height == screen.width;
    }

    const DisplayTransferRecord* FindDisplayTransferForScreen(
        const DekoScreenFrame& screen, const std::vector<DisplayTransferRecord>& transfers,
        const DisplayTransferRecord* skip = nullptr,
        bool allow_dimension_fallback = true) const {
        if (!screen.enabled) {
            return nullptr;
        }

        // Address match first (LCD framebuffer pointer == transfer output).
        // Reverse iteration so the most recent write wins.
        for (auto it = transfers.rbegin(); it != transfers.rend(); ++it) {
            if (&*it == skip) continue;
            if (it->output_address == screen.framebuffer_addr &&
                TransferMatchesScreen(*it, screen)) {
                return &*it;
            }
        }

        if (!allow_dimension_fallback) {
            return nullptr;
        }

        // Dimension fallback: when LCD addr is in a different memory region
        // (VRAM vs FCRAM mismatch we see in practice), match by physical
        // screen dims. Use FORWARD iteration here so we pick the OLDEST
        // matching transfer in the frame — for stereo 3D the game emits
        // left-eye first, right-eye second; both are 240×400 and the
        // reverse-pick was returning the right eye (wrong for mono present).
        // Also honour `skip` so a second screen looking up later doesn't
        // re-claim the transfer the first screen already got.
        for (auto it = transfers.begin(); it != transfers.end(); ++it) {
            if (&*it == skip) continue;
            if (TransferMatchesScreen(*it, screen)) {
                return &*it;
            }
        }

        return nullptr;
    }

    bool GetTransferSourceRect(const DisplayTransferRecord& transfer,
                               const DekoRenderTarget& source_target, u32& source_left,
                               u32& source_top, u32& source_width, u32& source_height) const {
        if (!source_target.ready || transfer.input_address < source_target.address ||
            transfer.input_width == 0 || transfer.output_width == 0 ||
            transfer.output_height == 0) {
            return false;
        }

        const auto input_format = static_cast<Pica::PixelFormat>(transfer.input_format);
        const u32 input_bpp = Pica::BytesPerPixel(input_format);
        const u32 row_bytes = transfer.input_width * input_bpp;
        if (row_bytes == 0) {
            return false;
        }

        const u64 source_offset = transfer.input_address - source_target.address;
        const u32 input_x = (source_offset % row_bytes) / input_bpp;
        const u32 input_y = source_offset / row_bytes;
        source_left = input_x;
        source_top = input_y;
        source_width = transfer.output_width;
        source_height = transfer.output_height;

        return source_width != 0 && source_height != 0 &&
               source_left < source_target.width && source_top < source_target.height &&
               source_left + source_width <= source_target.width &&
               source_top + source_height <= source_target.height;
    }

    static std::pair<u32, u32> GetTransferOutputSize(const DisplayTransferRecord& transfer) {
        u32 width = transfer.output_width;
        u32 height = transfer.output_height;
        if (transfer.scaling != Pica::DisplayTransferConfig::NoScale) {
            width /= 2;
        }
        if (transfer.scaling == Pica::DisplayTransferConfig::ScaleXY) {
            height /= 2;
        }
        return {width, height};
    }

    DekoRenderTarget* FindPicaTarget(PAddr address) {
        if (address == 0) {
            return nullptr;
        }

        const auto it = pica_color_targets.find(address);
        if (it != pica_color_targets.end() && it->second.ready) {
            return &it->second;
        }

        if (pica_color_target.ready && pica_color_target.address == address) {
            return &pica_color_target;
        }
        return nullptr;
    }

    DekoRenderTarget* FindPicaTargetForTransfer(const DisplayTransferRecord& transfer) {
        if (DekoRenderTarget* target = FindPicaTarget(transfer.target_address)) {
            return target;
        }

        const auto input_format = static_cast<Pica::PixelFormat>(transfer.input_format);
        const u32 input_bpp = Pica::BytesPerPixel(input_format);
        if (input_bpp == 0) {
            return nullptr;
        }

        for (auto& [address, target] : pica_color_targets) {
            if (!target.ready || address == 0) {
                continue;
            }
            const u64 size = static_cast<u64>(target.width) * target.height * input_bpp;
            if (size != 0 && transfer.input_address >= address &&
                transfer.input_address < address + size) {
                return &target;
            }
        }

        if (pica_color_target.ready) {
            const u64 size =
                static_cast<u64>(pica_color_target.width) * pica_color_target.height * input_bpp;
            if (size != 0 && transfer.input_address >= pica_color_target.address &&
                transfer.input_address < pica_color_target.address + size) {
                return &pica_color_target;
            }
        }
        return nullptr;
    }

    DekoRenderTarget* FindDisplayTarget(PAddr address) {
        if (address == 0) {
            return nullptr;
        }

        const auto it = display_targets.find(address);
        if (it == display_targets.end() || !it->second.ready) {
            return nullptr;
        }
        return &it->second;
    }

    DekoRenderTarget* FindDisplayTarget(const DekoScreenFrame& screen) {
        if (!screen.enabled) {
            return nullptr;
        }
        DekoRenderTarget* target = FindDisplayTarget(screen.framebuffer_addr);
        if (target == nullptr || target->width != screen.height || target->height != screen.width) {
            return nullptr;
        }
        return target;
    }

    bool MaterializeDisplayTransfer(const DisplayTransferRecord& transfer, u32 frame_count,
                                    const std::unordered_set<PAddr>& rendered_pica_targets) {
        DekoRenderTarget* source_target = FindPicaTargetForTransfer(transfer);
        const bool source_rendered =
            source_target != nullptr &&
            rendered_pica_targets.find(source_target->address) != rendered_pica_targets.end();
        if (source_target == nullptr || !source_rendered || transfer.output_address == 0) {
#ifdef __SWITCH__
            if (ShouldTraceDekoFrame(frame_count) || ShouldTraceDekoFrameSummary(frame_count)) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.display-materialize.reject "
                    "frame=%u reason=%s in=%08X out=%08X target=%08X source=%08X "
                    "known-targets=%zu rendered-targets=%zu",
                    frame_count, source_target == nullptr ? "no-source" : "stale-source",
                    static_cast<u32>(transfer.input_address),
                    static_cast<u32>(transfer.output_address),
                    static_cast<u32>(transfer.target_address),
                    source_target != nullptr ? static_cast<u32>(source_target->address) : 0U,
                    pica_color_targets.size(), rendered_pica_targets.size());
            }
#endif
            return false;
        }

        u32 source_left = 0;
        u32 source_top = 0;
        u32 source_width = 0;
        u32 source_height = 0;
        if (!GetTransferSourceRect(transfer, *source_target, source_left, source_top, source_width,
                                   source_height)) {
#ifdef __SWITCH__
            if (ShouldTraceDekoFrame(frame_count) || ShouldTraceDekoFrameSummary(frame_count)) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.display-materialize.reject "
                    "frame=%u reason=bad-source-rect in=%08X out=%08X source=%08X "
                    "source-size=%ux%u transfer=%ux%u",
                    frame_count, static_cast<u32>(transfer.input_address),
                    static_cast<u32>(transfer.output_address),
                    static_cast<u32>(source_target->address), source_target->width,
                    source_target->height, transfer.output_width, transfer.output_height);
            }
#endif
            return false;
        }

        const auto [output_width, output_height] = GetTransferOutputSize(transfer);
        if (output_width == 0 || output_height == 0) {
            return false;
        }
        const auto output_pixel_format = static_cast<Pica::PixelFormat>(transfer.output_format);
        const DkImageFormat output_format = DekoRenderTarget::MapPixelFormat(output_pixel_format);

        auto [it, inserted] = display_targets.try_emplace(transfer.output_address);
        DekoRenderTarget& target = it->second;
        if (!target.EnsureDkFormat(device, queue, transfer.output_address, output_width, output_height,
                                   output_format)) {
            if (inserted) {
                display_targets.erase(it);
            }
            return false;
        }

        dk::ImageView source_view{source_target->image};
        dk::ImageView target_view{target.image};
        const DkImageRect source_rect{source_left, source_top, 0, source_width, source_height, 1};
        const DkImageRect target_rect{0, 0, 0, output_width, output_height, 1};
        command_buffer.blitImage(source_view, source_rect, target_view, target_rect,
                                 DkBlitFlag_FilterNearest | DkBlitFlag_ModeBlit);
        command_buffer.barrier(DkBarrier_Full, DkInvalidateFlags_Image);

#ifdef __SWITCH__
        if (ShouldTraceDekoFrame(frame_count) || ShouldTraceDekoFrameSummary(frame_count)) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.display-materialize frame=%u out=%08X "
                "source-pica=%08X in=%08X src=%u,%u,%u,%u dst=%ux%u "
                "input-format=%u output-format=%u targets=%u",
                frame_count, static_cast<u32>(transfer.output_address),
                static_cast<u32>(source_target->address), static_cast<u32>(transfer.input_address),
                source_rect.x, source_rect.y, source_rect.width, source_rect.height, output_width,
                output_height, transfer.input_format, transfer.output_format,
                static_cast<u32>(display_targets.size()));
        }
#endif
        return true;
    }

    bool PresentLcdFrame(const DekoScreenFrame& top, const DekoScreenFrame& bottom, u32 frame_count,
                         const std::vector<PresentBatch>& batches,
                         const std::vector<DisplayTransferRecord>& display_transfers,
                         Memory::MemorySystem& memory, RasterizerDeko3D& rasterizer,
                         Pica::PicaCore& pica,
                         const DekoPicaTargetInfo& pica_target, const Pica::RegsInternal& live_regs,
                         const Pica::Shader::Generator::FSUniformData& fsu,
                         const Pica::Shader::Generator::VSUniformData& vsu) {
        bool using_cached_transfers = false;
        const std::vector<DisplayTransferRecord>* active_transfers = &display_transfers;
        if (!display_transfers.empty()) {
            cached_display_transfers = display_transfers;
            cached_display_transfer_age = 0;
        } else if (!cached_display_transfers.empty() && cached_display_transfer_age < 60) {
            active_transfers = &cached_display_transfers;
            using_cached_transfers = true;
            ++cached_display_transfer_age;
        }

        bool using_cached_batches = false;
        const std::vector<PresentBatch>* draw_batches = &batches;
        if (!batches.empty()) {
            cached_present_batches = batches;
            cached_present_age = 0;
        } else if (!cached_present_batches.empty() && cached_present_age < 60) {
            draw_batches = &cached_present_batches;
            using_cached_batches = true;
            ++cached_present_age;
        }

        const bool has_batch_target =
            std::any_of(draw_batches->begin(), draw_batches->end(),
                        [](const PresentBatch& batch) { return batch.target_enabled; });
        const bool can_draw_pica_target =
            (pica_target.enabled || has_batch_target) && top.enabled && top.width != 0 &&
            top.height != 0 && !draw_batches->empty();
        const bool allow_transfer_dimension_fallback = true;
        const DisplayTransferRecord* top_transfer = FindDisplayTransferForScreen(
            top, *active_transfers, nullptr, allow_transfer_dimension_fallback);
        // Pass top_transfer so the bottom lookup won't re-claim it (avoids
        // both screens picking the same 240×400 transfer when fallback hits).
        const DisplayTransferRecord* bottom_transfer = FindDisplayTransferForScreen(
            bottom, *active_transfers, top_transfer, allow_transfer_dimension_fallback);
#ifdef __SWITCH__
        // Log every 120 frames once we have transfers — captures the
        // steady-state routing (the previous "first 16 frames" cap fired
        // only before any rendering began and was useless).
        const bool transfer_pick_should_log =
            !active_transfers->empty() &&
            (transfer_pick_log_count < 4 || (frame_count % 120) == 0);
        if (transfer_pick_should_log) {
            ++transfer_pick_log_count;
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.transfer-pick frame=%u top-out=%08X "
                "top-in=%08X top-target=%08X top-dims=%ux%u top-batches=%u "
                "bottom-out=%08X bottom-in=%08X bottom-target=%08X bottom-dims=%ux%u "
                "bottom-batches=%u transfers=%zu "
                "cached-transfers=%u dim-fallback=%u top-fb=%08X bottom-fb=%08X "
                "top-dst=%u,%u,%u,%u "
                "bottom-dst=%u,%u,%u,%u",
                frame_count,
                top_transfer ? top_transfer->output_address : 0,
                top_transfer ? top_transfer->input_address : 0,
                top_transfer ? top_transfer->target_address : 0,
                top_transfer ? top_transfer->output_width : 0,
                top_transfer ? top_transfer->output_height : 0,
                top_transfer ? top_transfer->batch_count : 0,
                bottom_transfer ? bottom_transfer->output_address : 0,
                bottom_transfer ? bottom_transfer->input_address : 0,
                bottom_transfer ? bottom_transfer->target_address : 0,
                bottom_transfer ? bottom_transfer->output_width : 0,
                bottom_transfer ? bottom_transfer->output_height : 0,
                bottom_transfer ? bottom_transfer->batch_count : 0,
                active_transfers->size(), using_cached_transfers ? 1 : 0,
                allow_transfer_dimension_fallback ? 1 : 0,
                top.framebuffer_addr, bottom.framebuffer_addr,
                top.dst.left, top.dst.top, top.dst.right, top.dst.bottom,
                bottom.dst.left, bottom.dst.top, bottom.dst.right, bottom.dst.bottom);
        }
#endif
        if ((!can_draw_pica_target && !PrepareSource(top_source, top)) ||
            !PrepareSource(bottom_source, bottom)) {
            LOG_ERROR(Render, "Deko3D LCD source upload failed");
            return false;
        }

        const int slot = queue.acquireImage(swapchain);
        if (slot < 0 || slot >= static_cast<int>(NumFramebuffers)) {
            LOG_ERROR(Render, "Deko3D acquire image failed slot={}", slot);
            return false;
        }

        command_buffer.clear();
        command_buffer.addMemory(command_memory.block, 0, command_memory.size);
        present_vertex_frame_offset = 0;
        if (!EnsurePresentVertexBuffer(PresentVertexBufferSize)) {
            LOG_ERROR(Render, "Deko3D present vertex buffer creation failed");
            return false;
        }

        std::unordered_map<PAddr, std::vector<PresentBatch>> batches_by_target;
        std::unordered_map<PAddr, DekoPicaTargetInfo> target_infos;
        std::vector<PAddr> target_order;
        for (const PresentBatch& batch : *draw_batches) {
            DekoPicaTargetInfo batch_target = pica_target;
            if (batch.target_enabled) {
                batch_target.color_address = batch.target_color_address;
                batch_target.width = batch.target_width;
                batch_target.height = batch.target_height;
                batch_target.color_format = batch.target_color_format;
                batch_target.enabled = true;
            }
            if (!batch_target.enabled || batch_target.color_address == 0 || batch_target.width == 0 ||
                batch_target.height == 0) {
                continue;
            }

            auto [group_it, inserted] = batches_by_target.try_emplace(batch_target.color_address);
            if (inserted) {
                target_order.push_back(batch_target.color_address);
                target_infos.emplace(batch_target.color_address, batch_target);
            }
            group_it->second.push_back(batch);
        }

        bool rendered_pica_target = false;
        std::unordered_set<PAddr> rendered_pica_targets;
        u32 materialized_transfers = 0;
        std::unordered_set<PAddr> materialized_outputs;

        const bool can_stage_transfers = target_order.size() == 1 && !active_transfers->empty();
        if (can_stage_transfers) {
            const PAddr target_address = target_order.front();
            const auto group_it = batches_by_target.find(target_address);
            const auto info_it = target_infos.find(target_address);
            if (group_it != batches_by_target.end() && info_it != target_infos.end()) {
                const auto& group = group_it->second;
                std::size_t rendered_count = 0;
                bool target_ready = false;
                u32 staged_segment_clears = 0;

                for (const auto& transfer : *active_transfers) {
                    if (transfer.target_address != target_address) {
                        continue;
                    }

                    const std::size_t transfer_count =
                        std::min<std::size_t>(transfer.batch_count, group.size());
                    if (transfer_count > rendered_count) {
                        std::vector<PresentBatch> segment(group.begin() + rendered_count,
                                                          group.begin() + transfer_count);
                        const bool clear_segment =
                            !target_ready || transfer.input_address != transfer.target_address;
                        if (clear_segment) {
                            ++staged_segment_clears;
                        }
                        if (DrawPicaColorTarget(segment, top, frame_count, using_cached_batches,
                                                rasterizer, pica, info_it->second, live_regs, fsu,
                                                vsu, clear_segment)) {
                            target_ready = true;
                            rendered_pica_target = true;
                            rendered_pica_targets.insert(target_address);
                        }
                        rendered_count = transfer_count;
                    }

                    if (target_ready &&
                        MaterializeDisplayTransfer(transfer, frame_count, rendered_pica_targets)) {
                        ++materialized_transfers;
                        materialized_outputs.insert(transfer.output_address);
                    }
                }

                if (rendered_count < group.size()) {
                    std::vector<PresentBatch> segment(group.begin() + rendered_count, group.end());
                    if (DrawPicaColorTarget(segment, top, frame_count, using_cached_batches,
                                            rasterizer, pica, info_it->second, live_regs, fsu, vsu,
                                            !target_ready)) {
                        rendered_pica_target = true;
                        rendered_pica_targets.insert(target_address);
                    }
                }
#ifdef __SWITCH__
                if (ShouldTraceDekoFrameSummary(frame_count)) {
                    Azahar::Switch::AppendLogFormat(
                        nullptr,
                        "android-flow stage=deko3d.transfer-stage frame=%u target=%08X "
                        "batches=%u transfers=%u materialized=%u segment-clears=%u",
                        frame_count, static_cast<u32>(target_address),
                        static_cast<u32>(group.size()), static_cast<u32>(active_transfers->size()),
                        materialized_transfers, staged_segment_clears);
                }
#endif
            }
        } else {
            for (PAddr target_address : target_order) {
                const auto group_it = batches_by_target.find(target_address);
                const auto info_it = target_infos.find(target_address);
                if (group_it == batches_by_target.end() || info_it == target_infos.end()) {
                    continue;
                }
                if (DrawPicaColorTarget(group_it->second, top, frame_count, using_cached_batches,
                                        rasterizer, pica, info_it->second, live_regs, fsu, vsu)) {
                    rendered_pica_target = true;
                    rendered_pica_targets.insert(target_address);
                }
            }

            if (rendered_pica_target) {
                for (const auto& transfer : *active_transfers) {
                    if (MaterializeDisplayTransfer(transfer, frame_count, rendered_pica_targets)) {
                        ++materialized_transfers;
                        materialized_outputs.insert(transfer.output_address);
                    }
                }
            }
        }

        dk::ImageView framebuffer_view{framebuffers[slot]};
        const std::array<DkImageView const*, 1> render_targets{&framebuffer_view};
        command_buffer.bindRenderTargets(render_targets);

        command_buffer.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

        DekoRenderTarget* top_output = FindDisplayTarget(top);
        DekoRenderTarget* bottom_output = FindDisplayTarget(bottom);
        DekoRenderTarget* top_transfer_output =
            top_transfer != nullptr ? FindDisplayTarget(top_transfer->output_address) : nullptr;
        DekoRenderTarget* bottom_transfer_output =
            bottom_transfer != nullptr ? FindDisplayTarget(bottom_transfer->output_address) : nullptr;
        DekoRenderTarget* top_pica_source =
            top_transfer != nullptr ? FindPicaTargetForTransfer(*top_transfer)
                                    : FindPicaTarget(pica_target.color_address);
        DekoRenderTarget* bottom_pica_source =
            bottom_transfer != nullptr ? FindPicaTargetForTransfer(*bottom_transfer)
                                       : FindPicaTarget(pica_target.color_address);
        const bool top_pica_source_rendered =
            top_pica_source != nullptr &&
            rendered_pica_targets.find(top_pica_source->address) != rendered_pica_targets.end();
        const bool bottom_pica_source_rendered =
            bottom_pica_source != nullptr &&
            rendered_pica_targets.find(bottom_pica_source->address) != rendered_pica_targets.end();
        const bool top_output_fresh =
            top.framebuffer_addr != 0 &&
            materialized_outputs.find(top.framebuffer_addr) != materialized_outputs.end();
        const bool bottom_output_fresh =
            bottom.framebuffer_addr != 0 &&
            materialized_outputs.find(bottom.framebuffer_addr) != materialized_outputs.end();
        const bool top_transfer_output_fresh =
            top_transfer != nullptr &&
            materialized_outputs.find(top_transfer->output_address) != materialized_outputs.end();
        const bool bottom_transfer_output_fresh =
            bottom_transfer != nullptr &&
            materialized_outputs.find(bottom_transfer->output_address) != materialized_outputs.end();

        u32 top_present_path = 0;
        u32 bottom_present_path = 0;
        if (DisplayTargetMatchesScreen(top_output, top)) {
            // Match Vulkan semantics: present the framebuffer address selected by the LCD regs.
            // That target can be older than this frame's transfers because games double-buffer.
            DrawRenderTargetToScreen(*top_output, top, frame_count, 0, 0, 0, 0,
                                     top_output_fresh ? "top-output-fresh" : "top-output-cache");
            top_present_path = 2;
        } else if (top_transfer_output != nullptr && top_transfer_output_fresh) {
            // Display transfers are snapshots. Prefer them over the live PICA target because games
            // often draw another screen into the same source target after the transfer.
            DrawRenderTargetToScreen(*top_transfer_output, top, frame_count, 0, 0, 0, 0,
                                     "top-transfer-output");
            top_present_path = 1;
        } else if (top_pica_source_rendered) {
            u32 source_left = 0;
            u32 source_top = 0;
            u32 source_width = 0;
            u32 source_height = 0;
            if (top_transfer != nullptr &&
                GetTransferSourceRect(*top_transfer, *top_pica_source, source_left, source_top,
                                      source_width, source_height)) {
                DrawRenderTargetToScreen(*top_pica_source, top, frame_count, source_left,
                                         source_top, source_width, source_height, "top-transfer");
            } else {
                DrawRenderTargetToScreen(*top_pica_source, top, frame_count);
            }
            top_present_path = 3;
        } else {
            BlitSource(top_source, top, framebuffer_view);
            top_present_path = 4;
        }
        if (DisplayTargetMatchesScreen(bottom_output, bottom)) {
            DrawRenderTargetToScreen(
                *bottom_output, bottom, frame_count, 0, 0, 0, 0,
                bottom_output_fresh ? "bottom-output-fresh" : "bottom-output-cache");
            bottom_present_path = 2;
        } else if (bottom_transfer_output != nullptr && bottom_transfer_output_fresh) {
            // Use the transfer snapshot, not the final PICA target, to avoid showing whatever the
            // game drew into the shared source after this bottom-screen transfer.
            DrawRenderTargetToScreen(*bottom_transfer_output, bottom, frame_count, 0, 0, 0, 0,
                                     "bottom-transfer-output");
            bottom_present_path = 1;
        } else if (bottom_pica_source_rendered && bottom_transfer != nullptr) {
            u32 source_left = 0;
            u32 source_top = 0;
            u32 source_width = 0;
            u32 source_height = 0;
            if (GetTransferSourceRect(*bottom_transfer, *bottom_pica_source, source_left, source_top,
                                      source_width, source_height)) {
                DrawRenderTargetToScreen(*bottom_pica_source, bottom, frame_count, source_left,
                                         source_top, source_width, source_height,
                                         "bottom-transfer");
            } else {
                BlitSource(bottom_source, bottom, framebuffer_view);
            }
            bottom_present_path = 3;
        } else {
            BlitSource(bottom_source, bottom, framebuffer_view);
            bottom_present_path = 4;
        }

#ifdef __SWITCH__
        if (ShouldTraceDekoFrameSummary(frame_count)) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.frame-summary frame=%u rendered-pica=%u "
                "rendered-targets=%u pica-targets=%u batches=%u cached=%u transfers=%u "
                "top-rgb=%u bottom-rgb=%u top-transfer=%u "
                "bottom-transfer=%u materialized=%u targets=%u top-output=%u bottom-output=%u "
                "top-output-fresh=%u bottom-output-fresh=%u top-transfer-output=%u "
                "bottom-transfer-output=%u top-transfer-output-fresh=%u "
                "bottom-transfer-output-fresh=%u top-path=%u bottom-path=%u "
                "cached-transfers=%u",
                frame_count, rendered_pica_target ? 1 : 0,
                static_cast<u32>(rendered_pica_targets.size()),
                static_cast<u32>(pica_color_targets.size()), static_cast<u32>(draw_batches->size()),
                using_cached_batches ? 1 : 0, static_cast<u32>(active_transfers->size()),
                top.has_rgb ? 1 : 0, bottom.has_rgb ? 1 : 0, top_transfer != nullptr ? 1 : 0,
                bottom_transfer != nullptr ? 1 : 0, materialized_transfers,
                static_cast<u32>(display_targets.size()), top_output != nullptr ? 1 : 0,
                bottom_output != nullptr ? 1 : 0, top_output_fresh ? 1 : 0,
                bottom_output_fresh ? 1 : 0, top_transfer_output != nullptr ? 1 : 0,
                bottom_transfer_output != nullptr ? 1 : 0, top_transfer_output_fresh ? 1 : 0,
                bottom_transfer_output_fresh ? 1 : 0, top_present_path, bottom_present_path,
                using_cached_transfers ? 1 : 0);
        }
#endif

        if (PresentDebugOverlay) {
            for (const auto& batch : *draw_batches) {
                BindTextureForBatch(batch.texture_configs[0], rasterizer);
                DrawPresentVertices(batch.vertices, top.dst, frame_count, using_cached_batches,
                                    PresentOverlayAlpha, true, false, "overlay");
                if (PresentDebugMirrorBottom) {
                    DrawPresentVertices(batch.vertices, bottom.dst, frame_count, using_cached_batches,
                                        PresentOverlayAlpha, true, false, "overlay-bottom");
                }
            }
        }

#ifdef __SWITCH__
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.frame-submit step=finish-list.begin "
            "func=RendererDeko3D::Context::Present line=%u frame=%u slot=%u",
            static_cast<u32>(__LINE__), frame_count, slot);
#endif
        const DkCmdList command_list = command_buffer.finishList();
#ifdef __SWITCH__
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.frame-submit step=submit.begin "
            "func=RendererDeko3D::Context::Present line=%u frame=%u slot=%u list=%llu",
            static_cast<u32>(__LINE__), frame_count, slot,
            static_cast<unsigned long long>(command_list));
#endif
        queue.submitCommands(command_list);
#ifdef __SWITCH__
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.frame-submit step=submit.end "
            "func=RendererDeko3D::Context::Present line=%u frame=%u slot=%u",
            static_cast<u32>(__LINE__), frame_count, slot);
        if (DebugWaitIdleAfterFrameSubmit) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.frame-submit step=submit-wait.begin "
                "func=RendererDeko3D::Context::Present line=%u frame=%u slot=%u",
                static_cast<u32>(__LINE__), frame_count, slot);
        }
#endif
        if (DebugWaitIdleAfterFrameSubmit) {
            queue.waitIdle();
        }
#ifdef __SWITCH__
        if (DebugWaitIdleAfterFrameSubmit) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.frame-submit step=submit-wait.end "
                "func=RendererDeko3D::Context::Present line=%u frame=%u slot=%u",
                static_cast<u32>(__LINE__), frame_count, slot);
        }
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.frame-submit step=present.begin "
            "func=RendererDeko3D::Context::Present line=%u frame=%u slot=%u",
            static_cast<u32>(__LINE__), frame_count, slot);
#endif
        queue.presentImage(swapchain, slot);
#ifdef __SWITCH__
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.frame-submit step=present.end "
            "func=RendererDeko3D::Context::Present line=%u frame=%u slot=%u",
            static_cast<u32>(__LINE__), frame_count, slot);
        if (DebugWaitIdleAfterFrameSubmit) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.frame-submit step=present-wait.begin "
                "func=RendererDeko3D::Context::Present line=%u frame=%u slot=%u",
                static_cast<u32>(__LINE__), frame_count, slot);
        }
#endif
        if (DebugWaitIdleAfterFrameSubmit) {
            queue.waitIdle();
        }
#ifdef __SWITCH__
        if (DebugWaitIdleAfterFrameSubmit) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.frame-submit step=present-wait.end "
                "func=RendererDeko3D::Context::Present line=%u frame=%u slot=%u",
                static_cast<u32>(__LINE__), frame_count, slot);
        }
#endif
        return true;
    }

    /// Per-frame batch queue owned by Context (Phase 1 plumbing). The
    /// rasterizer pushes here via SubmitBatch instead of buffering in its
    /// own member; SwapBuffers drains it. Sets up the architecture so the
    /// next iteration can move per-batch GPU work into SubmitBatch itself
    /// (true vk_rasterizer-style immediate submission) without further
    /// interface changes.
    std::vector<PresentBatch> incoming_batches;

    void SubmitBatch(const PresentBatch& batch) override {
        incoming_batches.push_back(batch);
    }

    u32 SubmittedBatchCount() const override {
        return static_cast<u32>(incoming_batches.size());
    }

    std::vector<PresentBatch> ConsumeIncoming() {
        std::vector<PresentBatch> out;
        out.swap(incoming_batches);
        return out;
    }
};

RasterizerDeko3D::RasterizerDeko3D(Memory::MemorySystem& memory, Pica::PicaCore& pica,
                                   VideoCore::CustomTexManager& custom_tex_manager,
                                   VideoCore::RendererBase& renderer)
    : RasterizerAccelerated{memory, pica},
      res_cache{memory, custom_tex_manager, texture_runtime, regs, renderer} {}

RasterizerDeko3D::~RasterizerDeko3D() = default;

void RasterizerDeko3D::SetDekoContext(dk::Device device, dk::Queue queue,
                                      dk::CmdBuf command_buffer) {
    texture_runtime.SetContext(device, queue, command_buffer);
}

void RasterizerDeko3D::DrawTriangles() {
    if (vertex_batch.empty()) {
        return;
    }

    std::size_t queued_vertices = 0;
    for (const auto& batch : present_batches) {
        queued_vertices += batch.vertices.size();
    }
    const std::size_t available =
        MaxPresentVertices > queued_vertices ? MaxPresentVertices - queued_vertices : 0;
    const std::size_t count = std::min(available, vertex_batch.size());
    PresentBatch batch{};
    batch.textures = GetTextureInfos(regs);
    batch.texture_configs = GetTextureConfigs(regs);
    batch.texture = batch.textures[0];
    const auto& batch_framebuffer = regs.framebuffer.framebuffer;
    batch.target_color_address = batch_framebuffer.GetColorBufferPhysicalAddress();
    batch.target_depth_address = batch_framebuffer.GetDepthBufferPhysicalAddress();
    batch.target_width = batch_framebuffer.GetWidth();
    batch.target_height = batch_framebuffer.GetHeight();
    batch.target_color_format = static_cast<u32>(batch_framebuffer.color_format.Value());
    batch.target_enabled =
        batch.target_color_address != 0 && batch.target_width != 0 && batch.target_height != 0;
    // Snapshot the PICA fragment-shader configuration for this draw so the
    // renderer can compile + bind the matching runtime FS per-batch. Without
    // this, all batches in a frame share one FS based on END-of-frame regs,
    // producing visual flicker / wrong output when batches differ in TEV
    // stages, alpha test, blend, etc.
    batch.fs_config.emplace(regs);
    batch.fs_config_hash = batch.fs_config->Hash();
    // Make sure the rasterizer's cached uniform state matches PICA regs as
    // of this draw call, then snapshot. SyncDrawUniforms is a no-op on
    // unchanged state, so it's cheap.
    SyncDrawUniforms();
    batch.fs_uniform_data = fs_data;
    const auto& sc = regs.rasterizer.scissor_test;
    batch.fs_uniform_data.scissor_x1 = static_cast<int>(sc.x1.Value());
    batch.fs_uniform_data.scissor_y1 = static_cast<int>(sc.y1.Value());
    batch.fs_uniform_data.scissor_x2 = static_cast<int>(sc.x2.Value());
    batch.fs_uniform_data.scissor_y2 = static_cast<int>(sc.y2.Value());
    batch.vs_uniform_data = vs_data;
    // Snapshot output-merger registers. PICA games change blend mode per
    // sprite (icons, text, transparent overlays); a global frame-wide
    // default produces all-opaque white for alpha-only textures.
    {
        const auto& om = regs.framebuffer.output_merger;
        const auto& framebuffer = regs.framebuffer.framebuffer;
        auto& rs = batch.render_state;
        rs.blend_enable = om.alphablend_enable != 0;
        rs.blend_eq_rgb = static_cast<u32>(om.alpha_blending.blend_equation_rgb.Value());
        rs.blend_eq_a = static_cast<u32>(om.alpha_blending.blend_equation_a.Value());
        rs.src_factor_rgb = static_cast<u32>(om.alpha_blending.factor_source_rgb.Value());
        rs.dst_factor_rgb = static_cast<u32>(om.alpha_blending.factor_dest_rgb.Value());
        rs.src_factor_a = static_cast<u32>(om.alpha_blending.factor_source_a.Value());
        rs.dst_factor_a = static_cast<u32>(om.alpha_blending.factor_dest_a.Value());
        rs.depth_test_enable = om.depth_test_enable != 0 || om.depth_write_enable != 0;
        rs.depth_test_func = static_cast<u32>(
            om.depth_test_enable != 0 ? om.depth_test_func.Value()
                                      : Pica::FramebufferRegs::CompareFunc::Always);
        rs.depth_write_enable =
            framebuffer.allow_depth_stencil_write != 0 && om.depth_write_enable != 0;
        rs.color_write_mask =
            framebuffer.allow_color_write != 0
                ? static_cast<u8>((om.red_enable ? 0x1 : 0) | (om.green_enable ? 0x2 : 0) |
                                  (om.blue_enable ? 0x4 : 0) | (om.alpha_enable ? 0x8 : 0))
                : 0;
        rs.logic_op = static_cast<u32>(om.logic_op.Value());
        rs.blend_const = om.blend_const.raw;
        // Stencil snapshot. The fields share the union under PICA's
        // raw_func/raw_op layout; capture each cleanly.
        rs.stencil_enable = om.stencil_test.enable != 0;
        rs.stencil_test_func = static_cast<u32>(om.stencil_test.func.Value());
        rs.stencil_ref = static_cast<u8>(om.stencil_test.reference_value.Value());
        rs.stencil_input_mask = static_cast<u8>(om.stencil_test.input_mask.Value());
        rs.stencil_write_mask = static_cast<u8>(om.stencil_test.write_mask.Value());
        rs.stencil_action_fail =
            static_cast<u32>(om.stencil_test.action_stencil_fail.Value());
        rs.stencil_action_depth_fail =
            static_cast<u32>(om.stencil_test.action_depth_fail.Value());
        rs.stencil_action_depth_pass =
            static_cast<u32>(om.stencil_test.action_depth_pass.Value());
        rs.cull_mode = static_cast<u32>(regs.rasterizer.cull_mode.Value());
        rs.flip_viewport = framebuffer.IsFlipped();
        Common::Rectangle<s32> viewport_rect = regs.rasterizer.GetViewportRect();
        const s32 fb_width = static_cast<s32>(framebuffer.GetWidth());
        const s32 fb_height = static_cast<s32>(framebuffer.GetHeight());
        if (rs.flip_viewport) {
            viewport_rect = viewport_rect.VerticalMirror(fb_height);
        }
        const s32 viewport_width = viewport_rect.right - viewport_rect.left;
        const s32 viewport_height = viewport_rect.top - viewport_rect.bottom;
        rs.viewport_valid =
            fb_width > 0 && fb_height > 0 && viewport_width > 0 && viewport_height > 0;
        if (rs.viewport_valid) {
            rs.viewport_x = viewport_rect.left;
            rs.viewport_y = fb_height - viewport_rect.top;
            rs.viewport_width = viewport_width;
            rs.viewport_height = viewport_height;

            const s32 scissor_left = std::clamp(viewport_rect.left, 0, fb_width);
            const s32 scissor_right = std::clamp(viewport_rect.right, 0, fb_width);
            const s32 scissor_bottom = std::clamp(viewport_rect.bottom, 0, fb_height);
            const s32 scissor_top = std::clamp(viewport_rect.top, 0, fb_height);
            rs.scissor_x = static_cast<u32>(scissor_left);
            rs.scissor_y = static_cast<u32>(fb_height - scissor_top);
            rs.scissor_width = static_cast<u32>(std::max(0, scissor_right - scissor_left));
            rs.scissor_height = static_cast<u32>(std::max(0, scissor_top - scissor_bottom));
            rs.viewport_valid = rs.scissor_width != 0 && rs.scissor_height != 0;
        }
    }
    batch.vertices.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        const auto& vertex = vertex_batch[i];
        PresentVertex present{};
        present.position = {vertex.position.x, vertex.position.y, vertex.position.z,
                            vertex.position.w};
        present.color = {vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w};
        present.tex_coord0 = {vertex.tex_coord0.x, vertex.tex_coord0.y};
        present.tex_coord1 = {vertex.tex_coord1.x, vertex.tex_coord1.y};
        present.tex_coord2 = {vertex.tex_coord2.x, vertex.tex_coord2.y};
        present.tex_coord0_w = vertex.tex_coord0_w;
        present.normquat = {vertex.normquat.x, vertex.normquat.y, vertex.normquat.z,
                            vertex.normquat.w};
        present.view = {vertex.view.x, vertex.view.y, vertex.view.z};
        batch.vertices.emplace_back(present);
    }
#ifdef __SWITCH__
    const bool trace_render_target =
        DekoHotTrace && (render_target_log_count < 128 || render_target_log_count == 240 ||
                         (render_target_log_count % 480) == 0);
    if (trace_render_target) {
        const auto textures = regs.texturing.GetTextures();
        const auto& texture0 = textures[0];
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.pica-target.queue.begin "
            "func=RasterizerDeko3D::DrawTriangles line=%d draw=%u vertices=%u "
            "batch-vertices=%u queued-before=%u submitter=%u tex0=%08X tex0-size=%ux%u "
            "tex0-format=%u target=%08X target-size=%ux%u target-format=%u",
            __LINE__, render_target_log_count, static_cast<u32>(vertex_batch.size()),
            static_cast<u32>(batch.vertices.size()), static_cast<u32>(queued_vertices),
            batch_submitter != nullptr ? 1 : 0,
            static_cast<u32>(texture0.config.GetPhysicalAddress()),
            static_cast<u32>(texture0.config.width.Value()),
            static_cast<u32>(texture0.config.height.Value()), static_cast<u32>(texture0.format),
            static_cast<u32>(batch.target_color_address), batch.target_width, batch.target_height,
            batch.target_color_format);
    }
#endif
    if (!batch.vertices.empty()) {
        // Phase 1: when a submitter is wired, push the batch to the GPU
        // immediately so multi-pass scenes (3D content behind UI panels)
        // accumulate layer-by-layer instead of all collapsing onto whatever
        // batch happened to be last in the snapshot queue. Fallback path
        // (snapshot then replay) kept for backward compatibility.
        if (batch_submitter != nullptr) {
            batch_submitter->SubmitBatch(batch);
        } else {
            present_batches.emplace_back(std::move(batch));
        }
        queued_vertices += count;
    }

#ifdef __SWITCH__
    if (DekoHotTrace && !present_batches.empty() && present_batches.back().texture.enabled &&
        (texture_sample_log_count < 16 || (texture_sample_log_count % 2048) == 0)) {
        const auto& texture0 = present_batches.back().texture;
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.texture0-batch batch=%u vertices=%u "
            "addr=%08X size=%ux%u format=%u type=%u",
            texture_sample_log_count, static_cast<u32>(vertex_batch.size()),
            static_cast<u32>(texture0.address), texture0.width, texture0.height, texture0.format,
            texture0.type);
    }
    if (!present_batches.empty() && present_batches.back().texture.enabled) {
        ++texture_sample_log_count;
    }

    if (trace_render_target) {
        const auto& framebuffer = regs.framebuffer.framebuffer;
        const auto textures = regs.texturing.GetTextures();
        const auto& texture0 = textures[0];
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.pica-target func=RasterizerDeko3D::DrawTriangles "
            "line=%d draw=%u color=%08X depth=%08X size=%ux%u "
            "flip=%u color-format=%u depth-format=%u color-write=%u depth-write=%u "
            "tex0-enabled=%u tex0=%08X tex0-size=%ux%u tex0-format=%u tex0-type=%u "
            "vertices=%u queued=%u",
            __LINE__, render_target_log_count,
            static_cast<u32>(framebuffer.GetColorBufferPhysicalAddress()),
            static_cast<u32>(framebuffer.GetDepthBufferPhysicalAddress()), framebuffer.GetWidth(),
            framebuffer.GetHeight(), framebuffer.IsFlipped() ? 1 : 0,
            static_cast<u32>(framebuffer.color_format.Value()),
            static_cast<u32>(framebuffer.depth_format.Value()),
            static_cast<u32>(framebuffer.allow_color_write.Value()),
            static_cast<u32>(framebuffer.allow_depth_stencil_write.Value()),
            static_cast<u32>(texture0.enabled),
            static_cast<u32>(texture0.config.GetPhysicalAddress()),
            static_cast<u32>(texture0.config.width.Value()),
            static_cast<u32>(texture0.config.height.Value()),
            static_cast<u32>(texture0.format),
            static_cast<u32>(texture0.config.type.Value()), static_cast<u32>(vertex_batch.size()),
            static_cast<u32>(queued_vertices));
    }
    ++render_target_log_count;

    if (DekoHotTrace &&
        (fallback_draw_log_count < 8 || fallback_draw_log_count == 16 ||
         fallback_draw_log_count == 32 || (fallback_draw_log_count % 60) == 0)) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d-rasterizer.cpu-vs-batch "
            "func=RasterizerDeko3D::DrawTriangles line=%d count=%u vertices=%u queued=%u "
            "dropped=%u",
            __LINE__, fallback_draw_log_count, static_cast<u32>(vertex_batch.size()),
            static_cast<u32>(queued_vertices),
            static_cast<u32>(vertex_batch.size() - count));
    }
    ++fallback_draw_log_count;
#endif

    vertex_batch.clear();
#ifdef __SWITCH__
    if (trace_render_target) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.pica-target.end "
            "func=RasterizerDeko3D::DrawTriangles line=%d draw=%u queued=%u",
            __LINE__, render_target_log_count - 1, static_cast<u32>(queued_vertices));
    }
#endif
}

void RasterizerDeko3D::ClearAll(bool flush) {
    res_cache.ClearAll(flush);
    vertex_batch.clear();
    display_transfers.clear();
    present_batches.clear();
}

void RasterizerDeko3D::FlushAll() {
    res_cache.FlushAll();
}

void RasterizerDeko3D::FlushRegion(PAddr addr, u32 size) {
    res_cache.FlushRegion(addr, size);
}

void RasterizerDeko3D::InvalidateRegion(PAddr addr, u32 size) {
    res_cache.InvalidateRegion(addr, size);
}

void RasterizerDeko3D::FlushAndInvalidateRegion(PAddr addr, u32 size) {
    res_cache.FlushRegion(addr, size);
    res_cache.InvalidateRegion(addr, size);
}

bool RasterizerDeko3D::AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) {
    if (config.is_texture_copy || config.input_width == 0 || config.input_height == 0 ||
        config.output_width == 0 || config.output_height == 0) {
        return false;
    }

    if (config.input_format.Value() != Pica::PixelFormat::RGBA8 ||
        config.scaling.Value() != Pica::DisplayTransferConfig::NoScale || config.flip_vertically ||
        config.input_linear || config.dont_swizzle) {
        return false;
    }

    const auto& framebuffer = regs.framebuffer.framebuffer;
    const PAddr color_address = framebuffer.GetColorBufferPhysicalAddress();
    const PAddr input_address = config.GetPhysicalInputAddress();
    const PAddr output_address = config.GetPhysicalOutputAddress();
    const u32 input_bpp = Pica::BytesPerPixel(config.input_format);
    const u64 target_size =
        static_cast<u64>(framebuffer.GetWidth()) * framebuffer.GetHeight() * input_bpp;

    if (color_address == 0 || target_size == 0 || input_address < color_address ||
        input_address >= color_address + target_size || !memory.IsValidPhysicalAddress(output_address)) {
        return false;
    }

    DisplayTransferRecord record{
        input_address,
        output_address,
        config.input_width.Value(),
        config.input_height.Value(),
        config.output_width.Value(),
        config.output_height.Value(),
        static_cast<u32>(config.input_format.Value()),
        static_cast<u32>(config.output_format.Value()),
        config.flags,
        static_cast<u32>(config.scaling.Value()),
        color_address,
        framebuffer.GetWidth(),
        framebuffer.GetHeight(),
        static_cast<u32>(framebuffer.color_format.Value()),
        batch_submitter != nullptr ? batch_submitter->SubmittedBatchCount()
                                   : static_cast<u32>(present_batches.size()),
    };

    if (display_transfers.size() >= 64) {
        display_transfers.erase(display_transfers.begin());
    }
    display_transfers.push_back(record);

#ifdef __SWITCH__
    if (DekoHotTrace &&
        (display_transfer_log_count < 12 || display_transfer_log_count == 16 ||
         display_transfer_log_count == 32 || (display_transfer_log_count % 60) == 0)) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.display-transfer count=%u in=%08X out=%08X "
            "in-size=%ux%u out-size=%ux%u flags=%08X target=%08X target-size=%ux%u "
            "target-format=%u batches=%u",
            display_transfer_log_count, input_address, output_address, record.input_width,
            record.input_height, record.output_width, record.output_height, record.flags,
            color_address, framebuffer.GetWidth(), framebuffer.GetHeight(),
            record.target_color_format, record.batch_count);
    }
    ++display_transfer_log_count;
#endif

    return true;
}

bool RasterizerDeko3D::AccelerateDrawBatch(bool is_indexed) {
    if (draw_batch_log_count < 4) {
        LOG_INFO(Render, "android-flow stage=deko3d-rasterizer.draw-batch indexed={}",
                 is_indexed);
        ++draw_batch_log_count;
    }

    // The full Deko3D PICA shader path is not ready yet. Returning false forces the core to run the
    // PICA vertex shader on CPU, after which DrawTriangles uploads those transformed vertices to
    // Deko3D for rasterization instead of silently swallowing draw calls.
    return false;
}

TextureBinding RasterizerDeko3D::GetTextureBinding(const PresentTextureConfig& texture) {
#ifdef __SWITCH__
    const bool trace_texture_binding = ShouldTraceTextureBinding(texture);
    if (trace_texture_binding) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.stack step=texture-bind.begin "
            "func=RasterizerDeko3D::GetTextureBinding line=%d enabled=%u "
            "addr=%08X size=%ux%u format=%u type=%u",
            __LINE__, texture.enabled ? 1 : 0, TextureAddress(texture), TextureWidth(texture),
            TextureHeight(texture), static_cast<u32>(texture.format),
            texture.enabled ? static_cast<u32>(texture.config.type.Value()) : 0);
    }
#endif
    Surface* surface = &res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);
    Sampler* sampler = &res_cache.GetSampler(VideoCore::NULL_SAMPLER_ID);
#ifdef __SWITCH__
    if (trace_texture_binding) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.stack step=texture-bind.defaults "
            "func=RasterizerDeko3D::GetTextureBinding line=%d addr=%08X",
            __LINE__, TextureAddress(texture));
    }
#endif

    if (texture.enabled) {
        const auto type = texture.config.type.Value();
        if (type == Pica::TexturingRegs::TextureConfig::Texture2D ||
            type == Pica::TexturingRegs::TextureConfig::Projection2D) {
            const Pica::TexturingRegs::FullTextureConfig full_config{
                1U,
                texture.config,
                texture.format,
            };
#ifdef __SWITCH__
            if (trace_texture_binding) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.stack step=texture-bind.surface.begin "
                    "func=RasterizerDeko3D::GetTextureBinding line=%d addr=%08X "
                    "size=%ux%u format=%u",
                    __LINE__, TextureAddress(texture), TextureWidth(texture),
                    TextureHeight(texture), static_cast<u32>(texture.format));
            }
#endif
            surface = &res_cache.GetTextureSurface(full_config);
#ifdef __SWITCH__
            if (trace_texture_binding) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.stack step=texture-bind.surface.end "
                    "func=RasterizerDeko3D::GetTextureBinding line=%d addr=%08X ready=%u",
                    __LINE__, TextureAddress(texture), surface->IsImageReady() ? 1 : 0);
            }
#endif
            if (IsSupportedDekoSamplerConfig(texture.config)) {
#ifdef __SWITCH__
                if (trace_texture_binding) {
                    Azahar::Switch::AppendLogFormat(
                        nullptr,
                        "android-flow stage=deko3d.stack step=texture-bind.sampler.begin "
                        "func=RasterizerDeko3D::GetTextureBinding line=%d addr=%08X",
                        __LINE__, TextureAddress(texture));
                }
#endif
                sampler = &res_cache.GetSampler(texture.config);
#ifdef __SWITCH__
                if (trace_texture_binding) {
                    Azahar::Switch::AppendLogFormat(
                        nullptr,
                        "android-flow stage=deko3d.stack step=texture-bind.sampler.end "
                        "func=RasterizerDeko3D::GetTextureBinding line=%d addr=%08X",
                        __LINE__, TextureAddress(texture));
                }
#endif
            } else {
#ifdef __SWITCH__
                static std::atomic_uint sampler_fallback_log_count{0};
                if (sampler_fallback_log_count.fetch_add(1, std::memory_order_relaxed) < 256) {
                    Azahar::Switch::AppendLogFormat(
                        nullptr,
                        "android-flow stage=deko3d.stack step=texture-bind.sampler-fallback "
                        "func=RasterizerDeko3D::GetTextureBinding line=%d addr=%08X "
                        "size=%ux%u format=%u wrap=%u/%u filter=%u/%u/%u",
                        __LINE__, TextureAddress(texture), TextureWidth(texture),
                        TextureHeight(texture), static_cast<u32>(texture.format),
                        static_cast<u32>(texture.config.wrap_s.Value()),
                        static_cast<u32>(texture.config.wrap_t.Value()),
                        static_cast<u32>(texture.config.mag_filter.Value()),
                        static_cast<u32>(texture.config.min_filter.Value()),
                        static_cast<u32>(texture.config.mip_filter.Value()));
                }
#endif
            }
        }
    }

#ifdef __SWITCH__
    if (trace_texture_binding) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.stack step=texture-bind.end "
            "func=RasterizerDeko3D::GetTextureBinding line=%d enabled=%u "
            "addr=%08X size=%ux%u format=%u",
            __LINE__, texture.enabled ? 1 : 0, TextureAddress(texture), TextureWidth(texture),
            TextureHeight(texture), static_cast<u32>(texture.format));
    }
#endif
    dk::ImageView image_view = surface->ImageView();
    if (!surface->IsImageReady()) {
#ifdef __SWITCH__
        static std::atomic_uint image_not_ready_log_count{0};
        if (image_not_ready_log_count.fetch_add(1, std::memory_order_relaxed) < 256) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.stack step=texture-bind.image-not-ready "
                "func=RasterizerDeko3D::GetTextureBinding line=%d enabled=%u "
                "addr=%08X size=%ux%u format=%u type=%u",
                __LINE__, texture.enabled ? 1 : 0, TextureAddress(texture), TextureWidth(texture),
                TextureHeight(texture), static_cast<u32>(texture.format),
                texture.enabled ? static_cast<u32>(texture.config.type.Value()) : 0);
        }
#endif
        Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);
        image_view = null_surface.ImageView();
        surface = &null_surface;
    }

    return {
        .image_view = image_view,
        .sampler = sampler->Handle(),
        .valid = surface->IsImageReady(),
    };
}

bool RasterizerDeko3D::AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) {
    // Texture sampling now uses RasterizerCache-backed Deko images, but display-transfer writes still
    // need a complete Deko implementation before the shared cache can own them.
    return false;
}

bool RasterizerDeko3D::AccelerateFill(const Pica::MemoryFillConfig& config) {
    // Keep fills on the CPU path until Deko render-target ownership, clears, and invalidation are
    // handled together through RasterizerCache.
    return false;
}

std::vector<DisplayTransferRecord> RasterizerDeko3D::ConsumeDisplayTransfers() {
    std::vector<DisplayTransferRecord> transfers;
    transfers.swap(display_transfers);
    return transfers;
}

std::vector<PresentBatch> RasterizerDeko3D::ConsumePresentBatches() {
    std::vector<PresentBatch> batches;
    batches.swap(present_batches);
    return batches;
}

RendererDeko3D::RendererDeko3D(Core::System& system, Pica::PicaCore& pica_,
                               Frontend::EmuWindow& window)
    : RendererBase{system, window, nullptr}, memory{system.Memory()}, pica{pica_},
      rasterizer{system.Memory(), pica_, system.CustomTexManager(), *this} {}

RendererDeko3D::~RendererDeko3D() = default;

bool RendererDeko3D::EnsureContext() {
    if (context) {
        return true;
    }
    if (init_failed) {
        return false;
    }

    LOG_INFO(Render, "android-flow stage=deko3d.context.begin");
    auto next_context = std::make_unique<Context>();
    if (!next_context->Initialize()) {
        init_failed = true;
        LOG_ERROR(Render, "android-flow stage=deko3d.context.failed");
        return false;
    }
    context = std::move(next_context);
    rasterizer.SetDekoContext(context->device, context->queue, context->command_buffer);
    // Phase 1 plumbing: route DrawTriangles batches through Context's
    // BatchSubmitter override. Backfill point for true immediate
    // submission once the per-batch GPU work moves into SubmitBatch.
    rasterizer.SetBatchSubmitter(context.get());
    LOG_INFO(Render, "android-flow stage=deko3d.context.ready");

#ifdef __SWITCH__
    // One-shot self-test: prove uam is wired up by compiling a trivial GLSL vertex
    // shader at runtime and logging the resulting DKSH size. Expected size is the
    // same 512 bytes the offline-compiled present_vsh.dksh has — that confirms the
    // cross-compiled mesa stack reaches the same fixed point on hardware.
    static constexpr const char* kSelfTestGlsl =
        "#version 460\n"
        "layout(location = 0) in vec4 in_position;\n"
        "void main() { gl_Position = in_position; }\n";
    void* dksh_buf = nullptr;
    size_t dksh_size = 0;
    const int uam_rc =
        uam_compile_glsl(UAM_STAGE_VERTEX, kSelfTestGlsl, &dksh_buf, &dksh_size);
    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=deko3d.uam.selftest rc=%d size=%zu",
        uam_rc, dksh_size);
    if (dksh_buf) {
        uam_free(dksh_buf);
    }

    // Increment 3a: recompile the project's present_vsh.glsl at runtime via
    // uam and swap it in for the offline-compiled present_vertex_shader. If
    // frames keep rendering identically afterwards, the runtime pipeline is
    // a drop-in replacement for offline DKSH — which is the green light for
    // all subsequent PICA shaders to be runtime-compiled.
    static constexpr const char* kPresentVshSource =
        "#version 460\n"
        "\n"
        "layout (location = 0) in vec4 in_position;\n"
        "layout (location = 1) in vec4 in_color;\n"
        "layout (location = 2) in vec2 in_texcoord0;\n"
        "\n"
        "layout (location = 0) out vec4 out_color;\n"
        "layout (location = 1) out vec2 out_texcoord0;\n"
        "\n"
        "void main()\n"
        "{\n"
        "    gl_Position = vec4(in_position.x, in_position.y, -in_position.z, in_position.w);\n"
        "    out_color = in_color;\n"
        "    out_texcoord0 = in_texcoord0;\n"
        "}\n";

    if (uam_rc == 0 && dksh_size > 0) {
        void* shader_dksh = nullptr;
        size_t shader_dksh_size = 0;
        const int rc2 = uam_compile_glsl(UAM_STAGE_VERTEX, kPresentVshSource,
                                         &shader_dksh, &shader_dksh_size);
        u32 load_ok = 0;
        u32 placed_offset = 0;
        u32 swap_ok = 0;
        if (rc2 == 0 && shader_dksh != nullptr && shader_dksh_size > 0) {
            const u32 aligned_offset = AlignUp(
                context->shader_memory_cursor.load(std::memory_order_relaxed),
                DK_SHADER_CODE_ALIGNMENT);
            const u32 aligned_size =
                AlignUp(static_cast<u32>(shader_dksh_size), DK_SHADER_CODE_ALIGNMENT);
            if (aligned_offset + aligned_size <= context->shader_memory.size) {
                auto* code = static_cast<u8*>(context->shader_memory.block.getCpuAddr());
                std::memcpy(code + aligned_offset, shader_dksh, shader_dksh_size);
                context->shader_memory.block.flushCpuCache(aligned_offset, aligned_size);
                placed_offset = aligned_offset;
                context->shader_memory_cursor.store(aligned_offset + aligned_size,
                                                    std::memory_order_relaxed);
                load_ok = 1;

                // Actually replace the offline-compiled present vertex shader.
                // If this DKSH is interchangeable with the offline build, the
                // user will see no visual difference in the next frames.
                dk::Shader runtime_shader{};
                dk::ShaderMaker{context->shader_memory.block, aligned_offset}.initialize(
                    runtime_shader);
                context->present_vertex_shader = runtime_shader;
                swap_ok = 1;
            }
            uam_free(shader_dksh);
        }
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.uam.shader-load rc=%d size=%zu offset=%u load=%u swap=%u",
            rc2, shader_dksh_size, placed_offset, load_ok, swap_ok);
    }
#endif

    return true;
}

void RendererDeko3D::SwapBuffers() {
    system.perf_stats->StartSwap();

    if (frame_count == 0) {
        LOG_INFO(Render, "android-flow stage=deko3d.swap.begin");
    }

    if (EnsureContext()) {
        const auto load_screen = [this](u32 screen_index, const Pica::ColorFill& color_fill,
                                        const Common::Rectangle<u32>& dst,
                                        bool enabled) -> DekoScreenFrame {
            DekoScreenFrame screen{};
            screen.dst = dst;
            screen.enabled = enabled;
            if (!enabled) {
                return screen;
            }

            const u32 fb_id = screen_index == 2 ? 1 : 0;
            const auto& framebuffer = pica.regs.framebuffer_config[fb_id];
            const PAddr framebuffer_addr =
                framebuffer.active_fb == 0 ? framebuffer.address_left1 : framebuffer.address_left2;
            screen.framebuffer_addr = framebuffer_addr;
            const s32 bpp = Pica::BytesPerPixel(framebuffer.color_format);
            const u8* framebuffer_data = memory.GetPhysicalPointer(framebuffer_addr);
            if (bpp <= 0 || framebuffer.height == 0 || framebuffer.stride == 0) {
                return screen;
            }

            const u32 pixel_stride = framebuffer.stride / bpp;
            screen.width = framebuffer.height;
            screen.height = pixel_stride;
            screen.pixels.resize(static_cast<std::size_t>(screen.width) * screen.height * 4);

            u32 nonzero_pixels = 0;
            u32 first_rgb = 0;
            const auto fill_color =
                Common::Vec4<u8>(color_fill.color_r, color_fill.color_g, color_fill.color_b, 255);

            for (u32 y = 0; y < screen.height; ++y) {
                for (u32 x = 0; x < screen.width; ++x) {
                    const Common::Vec4 color = [&] {
                        if (framebuffer_data == nullptr) {
                            return fill_color;
                        }

                        const u8* pixel =
                            framebuffer_data + (x * pixel_stride + (pixel_stride - 1 - y)) * bpp;
                        switch (framebuffer.color_format) {
                        case Pica::PixelFormat::RGBA8:
                            return Common::Color::DecodeRGBA8(pixel);
                        case Pica::PixelFormat::RGB8:
                            return Common::Color::DecodeRGB8(pixel);
                        case Pica::PixelFormat::RGB565:
                            return Common::Color::DecodeRGB565(pixel);
                        case Pica::PixelFormat::RGB5A1:
                            return Common::Color::DecodeRGB5A1(pixel);
                        case Pica::PixelFormat::RGBA4:
                            return Common::Color::DecodeRGBA4(pixel);
                        }
                        UNREACHABLE();
                    }();

                    const std::size_t output_offset =
                        (static_cast<std::size_t>(y) * screen.width + x) * 4;
                    u8* dest = screen.pixels.data() + output_offset;
                    std::memcpy(dest, color.AsArray(), sizeof(color));
                    const bool nonzero = dest[0] != 0 || dest[1] != 0 || dest[2] != 0;
                    if (nonzero) {
                        ++nonzero_pixels;
                        if (first_rgb == 0) {
                            first_rgb = (static_cast<u32>(dest[0]) << 16) |
                                        (static_cast<u32>(dest[1]) << 8) |
                                        static_cast<u32>(dest[2]);
                        }
                    }
                    screen.has_rgb = screen.has_rgb || nonzero;
                }
            }

#ifdef __SWITCH__
            if (ShouldTraceDekoFrame(frame_count)) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=deko3d.lcd frame=%u screen=%u enabled=%u fb=%08X "
                    "data=%u size=%ux%u stride=%u bpp=%d format=%u color-fill=%u "
                    "fill-rgb=%02X%02X%02X nonzero=%u first-rgb=%06X",
                    frame_count, screen_index, enabled ? 1 : 0, framebuffer_addr,
                    framebuffer_data != nullptr ? 1 : 0, screen.width, screen.height,
                    framebuffer.stride, bpp, static_cast<u32>(framebuffer.color_format.Value()),
                    color_fill.is_enabled ? 1 : 0, color_fill.color_r, color_fill.color_g,
                    color_fill.color_b, nonzero_pixels, first_rgb);
            }
#endif

            return screen;
        };

        const auto& layout = render_window.GetFramebufferLayout();
        const auto& regs_lcd = pica.regs_lcd;
        const DekoScreenFrame top =
            load_screen(0, regs_lcd.color_fill_top, layout.top_screen, layout.top_screen_enabled);
        const DekoScreenFrame bottom = load_screen(2, regs_lcd.color_fill_bottom,
                                                   layout.bottom_screen,
                                                   layout.bottom_screen_enabled);
        auto display_transfers = rasterizer.ConsumeDisplayTransfers();
        // With Phase 1 plumbing, the rasterizer pushes batches into
        // Context via SubmitBatch. The legacy ConsumePresentBatches()
        // returns an empty vector now; we drain Context's queue instead.
        auto rasterizer_legacy = rasterizer.ConsumePresentBatches();
        auto present_batches = context->ConsumeIncoming();
        if (!rasterizer_legacy.empty()) {
            // Defensive: if for some reason the submitter wasn't wired,
            // fall back to the rasterizer's own queue so we keep rendering.
            present_batches.insert(present_batches.end(),
                                   std::make_move_iterator(rasterizer_legacy.begin()),
                                   std::make_move_iterator(rasterizer_legacy.end()));
        }

        const auto& pica_framebuffer = pica.regs.internal.framebuffer.framebuffer;
        const DekoPicaTargetInfo pica_target{
            pica_framebuffer.GetColorBufferPhysicalAddress(),
            pica_framebuffer.GetWidth(),
            pica_framebuffer.GetHeight(),
            static_cast<u32>(pica_framebuffer.color_format.Value()),
            pica_framebuffer.GetColorBufferPhysicalAddress() != 0 &&
                pica_framebuffer.GetWidth() != 0 && pica_framebuffer.GetHeight() != 0 &&
                pica_framebuffer.allow_color_write != 0,
        };
#ifdef __SWITCH__
        if (ShouldTraceDekoFrameSummary(frame_count)) {
            const auto& rast = pica.regs.internal.rasterizer;
            const auto vp_rect = rast.GetViewportRect();
            const float vp_size_x =
                Pica::f24::FromRaw(rast.viewport_size_x).ToFloat32();
            const float vp_size_y =
                Pica::f24::FromRaw(rast.viewport_size_y).ToFloat32();
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.pica-fb-snapshot frame=%u color=%08X depth=%08X "
                "size=%ux%u flip=%u color-fmt=%u depth-fmt=%u color-write=%u "
                "vp-corner=%d,%d vp-size-f=%f,%f vp-rect=%d,%d,%d,%d",
                frame_count,
                static_cast<u32>(pica_framebuffer.GetColorBufferPhysicalAddress()),
                static_cast<u32>(pica_framebuffer.GetDepthBufferPhysicalAddress()),
                pica_framebuffer.GetWidth(), pica_framebuffer.GetHeight(),
                pica_framebuffer.IsFlipped() ? 1 : 0,
                static_cast<u32>(pica_framebuffer.color_format.Value()),
                static_cast<u32>(pica_framebuffer.depth_format.Value()),
                static_cast<u32>(pica_framebuffer.allow_color_write.Value()),
                static_cast<int>(rast.viewport_corner.x),
                static_cast<int>(rast.viewport_corner.y), vp_size_x, vp_size_y,
                vp_rect.left, vp_rect.top, vp_rect.right, vp_rect.bottom);
        }
#endif
        // Synchronise PICA uniforms from current rasterizer state before
        // present — they reflect the last frame's PICA register snapshot.
        rasterizer.RuntimeSyncUniforms();
        const bool presented =
            context->PresentLcdFrame(top, bottom, frame_count, present_batches, display_transfers,
                                     memory, rasterizer, pica, pica_target, pica.regs.internal,
                                     rasterizer.GetFSUniformData(),
                                     rasterizer.GetVSUniformData());
        if (presented) {
            render_window.SwapBuffers();
        }
#ifdef __SWITCH__
        if (frame_count == 0) {
            Azahar::Switch::AppendLogFormat(
                nullptr, "android-flow stage=deko3d.window-submit frame=%u presented=%u",
                frame_count, presented ? 1U : 0U);
        }
#endif
        if (frame_count == 0) {
            LOG_INFO(Render, "android-flow stage=deko3d.swap.presented");
        }
    }

    ++frame_count;
    system.perf_stats->EndSwap();
    EndFrame();
}

} // namespace Deko3D
