// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_deko3d/renderer_deko3d.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
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
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_external.h"
#include "video_core/texture/texture_decode.h"

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
constexpr u32 CommandMemorySize = 1024 * 1024;
constexpr u32 ShaderMemorySize = 128 * 1024;
constexpr std::size_t MaxPresentVertices = 192 * 1024;
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

#ifdef __SWITCH__
bool ShouldTraceDekoFrame(u32 frame) {
    return DekoHotTrace && (frame < 8 || frame == 16 || frame == 32 || (frame % 60) == 0);
}

bool ShouldTraceDekoFrameSummary(u32 frame) {
    return DekoFrameSummaryTrace &&
           (frame < 4 || frame == 8 || frame == 16 || (frame % 120) == 0);
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
                                                 float alpha_cap) {
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

s32 WrapTextureCoord(Pica::TexturingRegs::TextureConfig::WrapMode mode, s32 coord, s32 size) {
    if (size <= 0) {
        return 0;
    }

    switch (mode) {
    case Pica::TexturingRegs::TextureConfig::ClampToEdge:
    case Pica::TexturingRegs::TextureConfig::ClampToEdge2:
    case Pica::TexturingRegs::TextureConfig::ClampToBorder:
    case Pica::TexturingRegs::TextureConfig::ClampToBorder2:
        return std::clamp(coord, 0, size - 1);
    case Pica::TexturingRegs::TextureConfig::Repeat:
    case Pica::TexturingRegs::TextureConfig::Repeat2:
    case Pica::TexturingRegs::TextureConfig::Repeat3:
        return ((coord % size) + size) % size;
    case Pica::TexturingRegs::TextureConfig::MirroredRepeat: {
        const s32 period = size * 2;
        s32 wrapped = ((coord % period) + period) % period;
        if (wrapped >= size) {
            wrapped = period - 1 - wrapped;
        }
        return wrapped;
    }
    default:
        return std::clamp(coord, 0, size - 1);
    }
}

bool IsSupportedPresentTextureType(u32 type) {
    return type == Pica::TexturingRegs::TextureConfig::Texture2D ||
           type == Pica::TexturingRegs::TextureConfig::Projection2D;
}

bool SampleTexture0Color(Memory::MemorySystem& memory, const Pica::RegsInternal& regs,
                         const Common::Vec2f& tex_coord0, float tex_coord0_w,
                         std::array<float, 4>& color) {
    const auto textures = regs.texturing.GetTextures();
    const auto& texture = textures[0];
    if (!texture.enabled || texture.config.address == 0) {
        return false;
    }

    const auto texture_type = texture.config.type.Value();
    float u = tex_coord0.x;
    float v = tex_coord0.y;
    if (texture_type == Pica::TexturingRegs::TextureConfig::Projection2D) {
        if (std::abs(tex_coord0_w) < 0.000001f) {
            return false;
        }
        u /= tex_coord0_w;
        v /= tex_coord0_w;
    } else if (texture_type != Pica::TexturingRegs::TextureConfig::Texture2D) {
        return false;
    }

    const s32 width = static_cast<s32>(texture.config.width.Value());
    const s32 height = static_cast<s32>(texture.config.height.Value());
    if (width <= 0 || height <= 0) {
        return false;
    }

    s32 s = static_cast<s32>(u * static_cast<float>(width));
    s32 t = static_cast<s32>(v * static_cast<float>(height));

    const auto wrap_s = texture.config.wrap_s.Value();
    const auto wrap_t = texture.config.wrap_t.Value();
    const bool use_border_s =
        wrap_s == Pica::TexturingRegs::TextureConfig::ClampToBorder ||
        (wrap_s == Pica::TexturingRegs::TextureConfig::ClampToBorder2 && s >= width);
    const bool use_border_t =
        wrap_t == Pica::TexturingRegs::TextureConfig::ClampToBorder ||
        (wrap_t == Pica::TexturingRegs::TextureConfig::ClampToBorder2 && t >= height);

    Common::Vec4<u8> sampled{};
    if ((use_border_s && (s < 0 || s >= width)) || (use_border_t && (t < 0 || t >= height))) {
        const auto border = texture.config.border_color;
        sampled = Common::MakeVec(border.r.Value(), border.g.Value(), border.b.Value(),
                                  border.a.Value())
                      .Cast<u8>();
    } else {
        s = WrapTextureCoord(wrap_s, s, width);
        t = height - 1 - WrapTextureCoord(wrap_t, t, height);
        const u8* texture_data = memory.GetPhysicalPointer(texture.config.GetPhysicalAddress());
        if (texture_data == nullptr) {
            return false;
        }
        const auto info = Pica::Texture::TextureInfo::FromPicaRegister(texture.config, texture.format);
        sampled = Pica::Texture::LookupTexture(texture_data, s, t, info);
    }

    color = {sampled.x / 255.0f, sampled.y / 255.0f, sampled.z / 255.0f, sampled.w / 255.0f};
    return true;
}

PresentTextureInfo GetTexture0Info(const Pica::RegsInternal& regs) {
    const auto texture0 = regs.texturing.GetTextures()[0];
    const bool has_texture = texture0.enabled != 0 && texture0.config.address != 0 &&
                             texture0.config.width.Value() != 0 &&
                             texture0.config.height.Value() != 0;
    return {
        texture0.config.GetPhysicalAddress(),
        static_cast<u32>(texture0.config.width.Value()),
        static_cast<u32>(texture0.config.height.Value()),
        static_cast<u32>(texture0.format),
        static_cast<u32>(texture0.config.type.Value()),
        has_texture,
    };
}

std::uint64_t TextureCacheKey(const PresentTextureInfo& info) {
    std::uint64_t key = static_cast<std::uint64_t>(info.address);
    key ^= static_cast<std::uint64_t>(info.width) << 32;
    key ^= static_cast<std::uint64_t>(info.height) << 43;
    key ^= static_cast<std::uint64_t>(info.format) << 54;
    key ^= static_cast<std::uint64_t>(info.type) << 60;
    return key;
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

    bool Ensure(dk::Device device, dk::Queue queue, Memory::MemorySystem& memory,
                const PresentTextureInfo& new_info) {
        if (ready && info.address == new_info.address && info.width == new_info.width &&
            info.height == new_info.height && info.format == new_info.format &&
            info.type == new_info.type) {
            return true;
        }
        if (!new_info.enabled || new_info.address == 0 || new_info.width == 0 ||
            new_info.height == 0) {
            return false;
        }

        queue.waitIdle();
        image = {};
        image_memory.Destroy();
        staging_memory.Destroy();
        info = new_info;
        ready = false;
        uploaded = false;

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(info.width, info.height)
            .initialize(layout);

        if (!image_memory.Create(device, layout.getSize(),
                                 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                     DkMemBlockFlags_Image)) {
            return false;
        }
        image.initialize(layout, image_memory.block, 0);

        const u32 byte_size = info.width * info.height * 4;
        if (!staging_memory.Create(device, byte_size,
                                   DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)) {
            return false;
        }

        auto* dst = static_cast<u8*>(staging_memory.block.getCpuAddr());
        const u8* texture_data = memory.GetPhysicalPointer(info.address);
        if (dst == nullptr || texture_data == nullptr) {
            return false;
        }

        Pica::TexturingRegs::TextureConfig config{};
        config.address.Assign(info.address / 8);
        config.width.Assign(info.width);
        config.height.Assign(info.height);
        config.type.Assign(static_cast<Pica::TexturingRegs::TextureConfig::TextureType>(info.type));
        const auto format = static_cast<Pica::TexturingRegs::TextureFormat>(info.format);
        const auto texture_info = Pica::Texture::TextureInfo::FromPicaRegister(config, format);
        for (u32 y = 0; y < info.height; ++y) {
            for (u32 x = 0; x < info.width; ++x) {
                const auto color =
                    Pica::Texture::LookupTexture(texture_data, x, info.height - 1 - y, texture_info);
                const std::size_t offset = (static_cast<std::size_t>(y) * info.width + x) * 4;
                dst[offset + 0] = color.x;
                dst[offset + 1] = color.y;
                dst[offset + 2] = color.z;
                dst[offset + 3] = color.w;
            }
        }
        staging_memory.block.flushCpuCache(0, byte_size);
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

struct DekoRenderTarget {
    dk::Image image{};
    DekoMemBlock image_memory{};
    PAddr address = 0;
    u32 width = 0;
    u32 height = 0;
    bool ready = false;

    bool Ensure(dk::Device device, dk::Queue queue, PAddr new_address, u32 new_width,
                u32 new_height) {
        if (address == new_address && width == new_width && height == new_height && ready) {
            return true;
        }

        queue.waitIdle();
        image = {};
        image_memory.Destroy();
        address = new_address;
        width = new_width;
        height = new_height;
        ready = false;

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
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

struct RendererDeko3D::Context {
    dk::UniqueDevice device{};
    dk::UniqueQueue queue{};
    dk::UniqueCmdBuf command_buffer{};
    dk::UniqueSwapchain swapchain{};
    DekoMemBlock command_memory{};
    DekoMemBlock shader_memory{};
    DekoMemBlock present_vertex_memory{};
    std::array<dk::Image, NumFramebuffers> framebuffers{};
    std::array<DekoMemBlock, NumFramebuffers> framebuffer_memory{};
    DekoSourceImage top_source{};
    DekoSourceImage bottom_source{};
    DekoRenderTarget pica_color_target{};
    DekoDescriptorSet image_descriptor_set{};
    DekoDescriptorSet sampler_descriptor_set{};
    DekoTextureImage white_texture{};
    std::unordered_map<std::uint64_t, DekoTextureImage> texture_cache{};
    dk::Shader present_vertex_shader{};
    dk::Shader present_fragment_shader{};
    u32 present_vertex_capacity = 0;
    u32 present_draw_log_count = 0;
    u32 present_nonempty_log_count = 0;
    u32 target_render_log_count = 0;
    std::vector<PresentBatch> cached_present_batches;
    u32 cached_present_age = 0;

    ~Context() {
        if (queue) {
            queue.waitIdle();
        }
    }

    bool Initialize() {
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

        if (!image_descriptor_set.Ensure(device, 1, sizeof(DkImageDescriptor)) ||
            !sampler_descriptor_set.Ensure(device, 1, sizeof(DkSamplerDescriptor)) ||
            !white_texture.EnsureWhite(device, queue)) {
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
        return LoadEmbeddedShader(present_vertex_shader, deko_present_vsh_dksh,
                                  deko_present_vsh_dksh_size, code_offset) &&
               LoadEmbeddedShader(present_fragment_shader, deko_present_fsh_dksh,
                                  deko_present_fsh_dksh_size, code_offset);
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

    bool BindTextureForBatch(const PresentTextureInfo& texture, Memory::MemorySystem& memory,
                             bool* uploaded_now_out = nullptr) {
        DekoTextureImage* image = &white_texture;
        if (texture.enabled) {
            if (!IsSupportedPresentTextureType(texture.type)) {
                return false;
            }
            auto [it, inserted] = texture_cache.try_emplace(TextureCacheKey(texture));
            if (it->second.Ensure(device, queue, memory, texture)) {
                image = &it->second;
            } else if (inserted) {
                texture_cache.erase(it);
                return false;
            } else {
                return false;
            }
        }

        const bool uploaded_now = image->UploadIfNeeded(command_buffer);
        if (uploaded_now_out != nullptr) {
            *uploaded_now_out = uploaded_now;
        }
        if (uploaded_now) {
            command_buffer.barrier(DkBarrier_None, DkInvalidateFlags_Image);
        }
        dk::ImageView image_view{image->image};
        dk::ImageDescriptor image_descriptor;
        image_descriptor.initialize(image_view);
        image_descriptor_set.Update(command_buffer, 0, image_descriptor);

        dk::Sampler sampler;
        sampler.setFilter(DkFilter_Linear, DkFilter_Linear);
        sampler.setWrapMode(DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge, DkWrapMode_ClampToEdge);
        dk::SamplerDescriptor sampler_descriptor;
        sampler_descriptor.initialize(sampler);
        sampler_descriptor_set.Update(command_buffer, 0, sampler_descriptor);
        image_descriptor_set.BindForImages(command_buffer);
        sampler_descriptor_set.BindForSamplers(command_buffer);
        command_buffer.bindTextures(DkStage_Fragment, 0, dkMakeTextureHandle(0, 0));
        return true;
    }

    void DrawPresentVertices(const std::vector<PresentVertex>& vertices,
                             const Common::Rectangle<u32>& dst, u32 frame_count,
                             bool using_cached_vertices, float alpha_cap, bool blend_enabled,
                             const char* pass_name) {
        const bool dst_valid = dst.right > dst.left && dst.bottom > dst.top;
        const auto drawable_vertices = BuildDrawableVertices(vertices, alpha_cap);

#ifdef __SWITCH__
        const bool has_vertices = !vertices.empty();
        const bool is_pica_target_pass = std::strcmp(pass_name, "pica-target") == 0;
        const bool should_log_present =
            DekoHotTrace &&
            (is_pica_target_pass
                 ? (present_draw_log_count < 8 || (present_draw_log_count % 2048) == 0)
                 : (present_draw_log_count < 16 || present_draw_log_count == 32 ||
                    (present_draw_log_count % 60) == 0 ||
                    (has_vertices && (present_nonempty_log_count < 32 ||
                                      (present_nonempty_log_count % 60) == 0))));
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
            return;
        }

        const u32 byte_size = static_cast<u32>(drawable_vertices.size() * sizeof(PresentVertex));
        if (!EnsurePresentVertexBuffer(byte_size)) {
            LOG_ERROR(Render, "Deko3D present vertex upload failed");
            return;
        }

        void* vertex_dst = present_vertex_memory.block.getCpuAddr();
        std::memcpy(vertex_dst, drawable_vertices.data(), byte_size);
        present_vertex_memory.block.flushCpuCache(0, byte_size);

        const DkViewport viewport{static_cast<float>(dst.left),
                                  static_cast<float>(dst.top),
                                  static_cast<float>(dst.right - dst.left),
                                  static_cast<float>(dst.bottom - dst.top),
                                  0.0f,
                                  1.0f};
        const DkScissor scissor{dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top};
        const std::array<DkShader const*, 2> shaders{&present_vertex_shader,
                                                     &present_fragment_shader};

        dk::RasterizerState rasterizer_state;
        dk::ColorState color_state;
        dk::ColorWriteState color_write_state;
        dk::BlendState blend_state;
        dk::DepthStencilState depth_stencil_state;
        color_state.setBlendEnable(0, blend_enabled);
        depth_stencil_state.setDepthTestEnable(false).setDepthWriteEnable(false);

        command_buffer.setViewports(0, {viewport});
        command_buffer.setScissors(0, {scissor});
        command_buffer.bindShaders(DkStageFlag_GraphicsMask, shaders);
        command_buffer.bindRasterizerState(rasterizer_state);
        command_buffer.bindColorState(color_state);
        command_buffer.bindColorWriteState(color_write_state);
        command_buffer.bindBlendStates(0, {blend_state});
        command_buffer.bindDepthStencilState(depth_stencil_state);
        command_buffer.bindVtxBuffer(0, present_vertex_memory.block.getGpuAddr(), byte_size);
        command_buffer.bindVtxAttribState(PresentVertexAttribState);
        command_buffer.bindVtxBufferState(PresentVertexBufferState);
        command_buffer.draw(DkPrimitive_Triangles, static_cast<u32>(drawable_vertices.size()), 1, 0,
                            0);

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
                             u32 frame_count, bool using_cached_batches, Memory::MemorySystem& memory,
                             const DekoPicaTargetInfo& pica_target) {
        if (!pica_target.enabled || batches.empty() || !top.enabled || top.width == 0 ||
            top.height == 0) {
            return false;
        }

        const bool changed = !pica_color_target.ready ||
                             pica_color_target.address != pica_target.color_address ||
                             pica_color_target.width != top.width ||
                             pica_color_target.height != top.height;
        if (!pica_color_target.Ensure(device, queue, pica_target.color_address, top.width,
                                      top.height)) {
            LOG_ERROR(Render, "Deko3D PICA color target creation failed");
            return false;
        }

        dk::ImageView target_view{pica_color_target.image};
        const std::array<DkImageView const*, 1> render_targets{&target_view};
        command_buffer.bindRenderTargets(render_targets);
        command_buffer.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

        const Common::Rectangle<u32> target_rect{0, 0, pica_color_target.width,
                                                 pica_color_target.height};
        u32 vertex_count = 0;
        u32 textured_batches = 0;
        u32 uploaded_textures = 0;
        u32 skipped_texture_batches = 0;
        u32 drawn_batches = 0;
        for (const auto& batch : batches) {
            vertex_count += static_cast<u32>(batch.vertices.size());
            if (batch.texture.enabled) {
                ++textured_batches;
            }
            bool uploaded_now = false;
            if (!BindTextureForBatch(batch.texture, memory, &uploaded_now)) {
                ++skipped_texture_batches;
                continue;
            }
            if (uploaded_now) {
                ++uploaded_textures;
            }
            DrawPresentVertices(batch.vertices, target_rect, frame_count, using_cached_batches, 1.0f,
                                false, "pica-target");
            ++drawn_batches;
        }
        command_buffer.barrier(DkBarrier_Tiles, DkInvalidateFlags_Image);

#ifdef __SWITCH__
        if (DekoHotTrace &&
            (changed || target_render_log_count < 16 || ShouldTraceDekoFrame(frame_count) ||
             (target_render_log_count % 60) == 0)) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.target-render frame=%u changed=%u pica=%08X "
                "pica-size=%ux%u image=%ux%u batches=%u drawn=%u textured=%u skipped=%u "
                "uploaded=%u cache=%u vertices=%u cached=%u",
                frame_count, changed ? 1 : 0, static_cast<u32>(pica_target.color_address),
                pica_target.width, pica_target.height, pica_color_target.width,
                pica_color_target.height, static_cast<u32>(batches.size()), drawn_batches,
                textured_batches, skipped_texture_batches, uploaded_textures,
                static_cast<u32>(texture_cache.size()), vertex_count, using_cached_batches ? 1 : 0);
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

    const DisplayTransferRecord* FindDisplayTransferForScreen(
        const DekoScreenFrame& screen, const std::vector<DisplayTransferRecord>& transfers) const {
        if (!screen.enabled) {
            return nullptr;
        }

        for (auto it = transfers.rbegin(); it != transfers.rend(); ++it) {
            if (it->output_address == screen.framebuffer_addr) {
                return &*it;
            }
        }

        for (auto it = transfers.rbegin(); it != transfers.rend(); ++it) {
            if (it->output_width == screen.height && it->output_height == screen.width) {
                return &*it;
            }
        }

        return nullptr;
    }

    bool GetTransferSourceRect(const DisplayTransferRecord& transfer,
                               const DekoPicaTargetInfo& pica_target, u32& source_left,
                               u32& source_top, u32& source_width, u32& source_height) const {
        if (!pica_color_target.ready || transfer.input_address < pica_target.color_address ||
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

        const u32 source_offset = transfer.input_address - pica_target.color_address;
        const u32 input_x = (source_offset % row_bytes) / input_bpp;
        const u32 input_y = source_offset / row_bytes;
        const bool rotated_target = pica_color_target.width == pica_target.height &&
                                    pica_color_target.height == pica_target.width;

        if (rotated_target) {
            source_left = input_y;
            source_top = input_x;
            source_width = transfer.output_height;
            source_height = transfer.output_width;
        } else {
            source_left = input_x;
            source_top = input_y;
            source_width = transfer.output_width;
            source_height = transfer.output_height;
        }

        return source_left < pica_color_target.width && source_top < pica_color_target.height &&
               source_width != 0 && source_height != 0;
    }

    bool PresentLcdFrame(const DekoScreenFrame& top, const DekoScreenFrame& bottom, u32 frame_count,
                         const std::vector<PresentBatch>& batches,
                         const std::vector<DisplayTransferRecord>& display_transfers,
                         Memory::MemorySystem& memory, const DekoPicaTargetInfo& pica_target) {
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

        const bool can_draw_pica_target =
            pica_target.enabled && top.enabled && top.width != 0 && top.height != 0 &&
            !draw_batches->empty();
        const DisplayTransferRecord* top_transfer =
            FindDisplayTransferForScreen(top, display_transfers);
        const DisplayTransferRecord* bottom_transfer =
            FindDisplayTransferForScreen(bottom, display_transfers);
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

        const bool rendered_pica_target = DrawPicaColorTarget(
            *draw_batches, top, frame_count, using_cached_batches, memory, pica_target);

        dk::ImageView framebuffer_view{framebuffers[slot]};
        const std::array<DkImageView const*, 1> render_targets{&framebuffer_view};
        command_buffer.bindRenderTargets(render_targets);

        const bool has_frame = (top.enabled && top.has_rgb) || (bottom.enabled && bottom.has_rgb);
        const float phase = static_cast<float>((frame_count / 30) % 3);
        const float r = !has_frame && phase == 0.0f ? 0.08f : 0.0f;
        const float g = !has_frame && phase == 1.0f ? 0.08f : 0.0f;
        const float b = !has_frame && phase == 2.0f ? 0.08f : 0.0f;
        command_buffer.clearColor(0, DkColorMask_RGBA, r, g, b, 1.0f);

        if (rendered_pica_target) {
            u32 source_left = 0;
            u32 source_top = 0;
            u32 source_width = 0;
            u32 source_height = 0;
            if (top_transfer != nullptr &&
                GetTransferSourceRect(*top_transfer, pica_target, source_left, source_top,
                                      source_width, source_height)) {
                BlitRenderTarget(pica_color_target, top, framebuffer_view, frame_count, source_left,
                                 source_top, source_width, source_height, "top-transfer");
            } else {
                BlitRenderTarget(pica_color_target, top, framebuffer_view, frame_count);
            }
        } else {
            BlitSource(top_source, top, framebuffer_view);
        }
        if (rendered_pica_target && bottom_transfer != nullptr) {
            u32 source_left = 0;
            u32 source_top = 0;
            u32 source_width = 0;
            u32 source_height = 0;
            if (GetTransferSourceRect(*bottom_transfer, pica_target, source_left, source_top,
                                      source_width, source_height)) {
                BlitRenderTarget(pica_color_target, bottom, framebuffer_view, frame_count,
                                 source_left, source_top, source_width, source_height,
                                 "bottom-transfer");
            } else {
                BlitSource(bottom_source, bottom, framebuffer_view);
            }
        } else {
            BlitSource(bottom_source, bottom, framebuffer_view);
        }

#ifdef __SWITCH__
        if (ShouldTraceDekoFrameSummary(frame_count)) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=deko3d.frame-summary frame=%u rendered-pica=%u "
                "batches=%u cached=%u transfers=%u top-rgb=%u bottom-rgb=%u top-transfer=%u "
                "bottom-transfer=%u",
                frame_count, rendered_pica_target ? 1 : 0, static_cast<u32>(draw_batches->size()),
                using_cached_batches ? 1 : 0, static_cast<u32>(display_transfers.size()),
                top.has_rgb ? 1 : 0, bottom.has_rgb ? 1 : 0, top_transfer != nullptr ? 1 : 0,
                bottom_transfer != nullptr ? 1 : 0);
        }
#endif

        if (PresentDebugOverlay) {
            for (const auto& batch : *draw_batches) {
                BindTextureForBatch(batch.texture, memory);
                DrawPresentVertices(batch.vertices, top.dst, frame_count, using_cached_batches,
                                    PresentOverlayAlpha, true, "overlay");
                if (PresentDebugMirrorBottom) {
                    DrawPresentVertices(batch.vertices, bottom.dst, frame_count, using_cached_batches,
                                        PresentOverlayAlpha, true, "overlay-bottom");
                }
            }
        }

        queue.submitCommands(command_buffer.finishList());
        queue.presentImage(swapchain, slot);
        return true;
    }
};

RasterizerDeko3D::RasterizerDeko3D(Memory::MemorySystem& memory, Pica::PicaCore& pica)
    : RasterizerAccelerated{memory, pica} {}

RasterizerDeko3D::~RasterizerDeko3D() = default;

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
    batch.texture = GetTexture0Info(regs);
    const bool texture0_projection =
        batch.texture.enabled &&
        batch.texture.type == Pica::TexturingRegs::TextureConfig::Projection2D;
    batch.vertices.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& vertex = vertex_batch[i];
        PresentVertex present{};
        present.position = {vertex.position.x, vertex.position.y, vertex.position.z,
                            vertex.position.w};
        present.color = {vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w};
        float tex0_s = vertex.tex_coord0.x;
        float tex0_t = vertex.tex_coord0.y;
        if (texture0_projection && std::abs(vertex.tex_coord0_w) >= 0.000001f) {
            tex0_s /= vertex.tex_coord0_w;
            tex0_t /= vertex.tex_coord0_w;
        }
        present.tex_coord0 = {tex0_s, tex0_t};
        batch.vertices.emplace_back(present);
    }
    if (!batch.vertices.empty()) {
        present_batches.emplace_back(std::move(batch));
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

    if (DekoHotTrace &&
        (render_target_log_count < 32 || render_target_log_count == 60 ||
         (render_target_log_count % 120) == 0)) {
        const auto& framebuffer = regs.framebuffer.framebuffer;
        const auto textures = regs.texturing.GetTextures();
        const auto& texture0 = textures[0];
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=deko3d.pica-target draw=%u color=%08X depth=%08X size=%ux%u "
            "flip=%u color-format=%u depth-format=%u color-write=%u depth-write=%u "
            "tex0-enabled=%u tex0=%08X tex0-size=%ux%u tex0-format=%u tex0-type=%u "
            "vertices=%u queued=%u",
            render_target_log_count,
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
            "android-flow stage=deko3d-rasterizer.cpu-vs-batch count=%u vertices=%u queued=%u "
            "dropped=%u",
            fallback_draw_log_count, static_cast<u32>(vertex_batch.size()),
            static_cast<u32>(queued_vertices),
            static_cast<u32>(vertex_batch.size() - count));
    }
    ++fallback_draw_log_count;
#endif

    vertex_batch.clear();
}

void RasterizerDeko3D::ClearAll(bool flush) {
    vertex_batch.clear();
    display_transfers.clear();
    present_batches.clear();
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
    const u32 target_size = framebuffer.GetWidth() * framebuffer.GetHeight() * input_bpp;

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
            "in-size=%ux%u out-size=%ux%u flags=%08X target=%08X target-size=%ux%u",
            display_transfer_log_count, input_address, output_address, record.input_width,
            record.input_height, record.output_width, record.output_height, record.flags,
            color_address, framebuffer.GetWidth(), framebuffer.GetHeight());
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
      rasterizer{system.Memory(), pica_} {}

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
    LOG_INFO(Render, "android-flow stage=deko3d.context.ready");
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
        auto present_batches = rasterizer.ConsumePresentBatches();

        const auto& pica_framebuffer = pica.regs.internal.framebuffer.framebuffer;
        const DekoPicaTargetInfo pica_target{
            pica_framebuffer.GetColorBufferPhysicalAddress(),
            pica_framebuffer.GetWidth(),
            pica_framebuffer.GetHeight(),
            pica_framebuffer.GetColorBufferPhysicalAddress() != 0 &&
                pica_framebuffer.GetWidth() != 0 && pica_framebuffer.GetHeight() != 0 &&
                pica_framebuffer.allow_color_write != 0,
        };
        context->PresentLcdFrame(top, bottom, frame_count, present_batches, display_transfers,
                                 memory, pica_target);
        if (frame_count == 0) {
            LOG_INFO(Render, "android-flow stage=deko3d.swap.presented");
        }
    }

    ++frame_count;
    system.perf_stats->EndSwap();
    EndFrame();
}

} // namespace Deko3D
