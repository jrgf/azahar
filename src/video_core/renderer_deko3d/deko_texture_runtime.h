// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <deko3d.hpp>

#include <fmt/format.h>

#include "common/vector_math.h"
#include "video_core/rasterizer_cache/framebuffer_base.h"
#include "video_core/rasterizer_cache/rasterizer_cache.h"
#include "video_core/rasterizer_cache/sampler_params.h"
#include "video_core/rasterizer_cache/surface_base.h"

namespace VideoCore {
struct Material;
}

namespace Deko3D {

class Surface;

class MemBlock {
public:
    MemBlock() = default;
    ~MemBlock();

    MemBlock(const MemBlock&) = delete;
    MemBlock& operator=(const MemBlock&) = delete;

    MemBlock(MemBlock&& other) noexcept;
    MemBlock& operator=(MemBlock&& other) noexcept;

    bool Create(dk::Device device, u32 requested_size, u32 flags);
    void Destroy();

    dk::MemBlock block{};
    u32 size = 0;
};

class TextureRuntime {
    friend class Surface;

public:
    TextureRuntime() = default;
    TextureRuntime(dk::Device device, dk::Queue queue, dk::CmdBuf command_buffer);
    ~TextureRuntime();

    void SetContext(dk::Device device, dk::Queue queue, dk::CmdBuf command_buffer);

    u32 RemoveThreshold();
    void Finish();
    VideoCore::StagingData FindStaging(u32 size, bool upload);

    bool Reinterpret(Surface& source, Surface& dest, const VideoCore::TextureCopy& copy);
    bool ClearTexture(Surface& surface, const VideoCore::TextureClear& clear);
    bool CopyTextures(Surface& source, Surface& dest, std::span<const VideoCore::TextureCopy> copies);
    bool CopyTextures(Surface& source, Surface& dest, const VideoCore::TextureCopy& copy) {
        return CopyTextures(source, dest, std::array{copy});
    }
    bool BlitTextures(Surface& source, Surface& dest, const VideoCore::TextureBlit& blit);
    void GenerateMipmaps(Surface& surface);
    bool NeedsConversion(const Surface& surface) const;
    bool HasContext() const;

private:
    dk::Device device{};
    dk::Queue queue{};
    dk::CmdBuf command_buffer{};
    std::vector<u8> staging_buffer;
};

class Surface : public VideoCore::SurfaceBase {
    friend class TextureRuntime;

public:
    explicit Surface(TextureRuntime& runtime, const VideoCore::SurfaceParams& params,
                     const VideoCore::SurfaceFlagBits& initial_flag_bits = {});
    explicit Surface(TextureRuntime& runtime, const VideoCore::SurfaceBase& surface,
                     const VideoCore::Material* material);
    ~Surface();

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    Surface(Surface&& other) noexcept;
    Surface& operator=(Surface&& other) noexcept;

    [[nodiscard]] dk::ImageView ImageView(u32 level = 0);
    [[nodiscard]] dk::Image& Image() noexcept {
        return image;
    }
    [[nodiscard]] bool IsImageReady() const noexcept {
        return image_ready;
    }

    void Upload(const VideoCore::BufferTextureCopy& upload, const VideoCore::StagingData& staging);
    void UploadCustom(const VideoCore::Material* material, u32 level);
    void Download(const VideoCore::BufferTextureCopy& download,
                  const VideoCore::StagingData& staging);
    void ScaleUp(u32 new_scale);
    u32 GetInternalBytesPerPixel() const;

private:
    void AllocateStorage();
    bool EnsureImage();
    MemBlock* AllocateUploadBlock(u32 byte_size);
    bool UploadImageRegion(u32 level, u32 x, u32 y, u32 width, u32 height, const u8* src,
                           u32 byte_size);
    void UploadImageIfNeeded();

    TextureRuntime* runtime = nullptr;
    dk::Image image{};
    MemBlock image_memory{};
    std::vector<MemBlock> upload_retire_blocks{};
    std::vector<u8> pixels;
    bool image_ready = false;
    bool image_dirty = true;
};

class Framebuffer : public VideoCore::FramebufferParams {
public:
    explicit Framebuffer(TextureRuntime& runtime, const VideoCore::FramebufferParams& params,
                         Surface* color, Surface* depth_stencil);
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    Framebuffer(Framebuffer&&) noexcept = default;
    Framebuffer& operator=(Framebuffer&&) noexcept = default;

    u32 Scale() const noexcept {
        return res_scale;
    }

    Surface* ColorSurface() const noexcept {
        return color;
    }

private:
    Surface* color = nullptr;
    Surface* depth_stencil = nullptr;
    u32 res_scale = 1;
};

class Sampler {
public:
    explicit Sampler(TextureRuntime& runtime, VideoCore::SamplerParams params);
    ~Sampler();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    Sampler(Sampler&&) noexcept = default;
    Sampler& operator=(Sampler&&) noexcept = default;

    [[nodiscard]] const dk::Sampler& Handle() const noexcept {
        return sampler;
    }

private:
    dk::Sampler sampler{};
};

class DebugScope {
public:
    template <typename... T>
    explicit DebugScope(TextureRuntime& runtime, Common::Vec4f color,
                        fmt::format_string<T...> format, T... args)
        : DebugScope{runtime, color, fmt::format(format, std::forward<T>(args)...)} {}
    explicit DebugScope(TextureRuntime& runtime, Common::Vec4f color, std::string_view label);
    ~DebugScope();
};

struct Traits {
    using Runtime = Deko3D::TextureRuntime;
    using Surface = Deko3D::Surface;
    using Sampler = Deko3D::Sampler;
    using Framebuffer = Deko3D::Framebuffer;
    using DebugScope = Deko3D::DebugScope;
};

using RasterizerCache = VideoCore::RasterizerCache<Traits>;

} // namespace Deko3D
