// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_deko3d/deko_texture_runtime.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "video_core/custom_textures/material.h"
#include "video_core/renderer_deko3d/pica_to_deko.h"
#include "video_core/rasterizer_cache/pixel_format.h"

#ifdef __SWITCH__
namespace Azahar::Switch {
bool AppendLogFormat(int* error_out, const char* format, ...);
}
#endif

namespace Deko3D {
namespace {

u32 AlignUp(u32 value, u32 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

u32 RectLeft(const Common::Rectangle<u32>& rect) {
    return std::min(rect.left, rect.right);
}

u32 RectRight(const Common::Rectangle<u32>& rect) {
    return std::max(rect.left, rect.right);
}

u32 RectTop(const Common::Rectangle<u32>& rect) {
    return std::min(rect.top, rect.bottom);
}

u32 RectBottom(const Common::Rectangle<u32>& rect) {
    return std::max(rect.top, rect.bottom);
}

u8 FloatToByte(float value) {
    return static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}

struct FormatInfo {
    DkImageFormat format = DkImageFormat_None;
    u32 bytes_per_pixel = 0;
    bool needs_conversion = false;
};

FormatInfo GetFormatInfo(VideoCore::PixelFormat format) {
    using VideoCore::PixelFormat;
    switch (format) {
    case PixelFormat::RGBA8:
        return {DkImageFormat_RGBA8_Unorm, 4, true};
    case PixelFormat::RGB8:
        return {DkImageFormat_RGBA8_Unorm, 4, true};
    case PixelFormat::RGB5A1:
        return {DkImageFormat_RGB5A1_Unorm, 2, false};
    case PixelFormat::RGB565:
        return {DkImageFormat_RGB565_Unorm, 2, false};
    case PixelFormat::RGBA4:
        return {DkImageFormat_RGBA4_Unorm, 2, false};
    case PixelFormat::IA8:
    case PixelFormat::RG8:
    case PixelFormat::I8:
    case PixelFormat::A8:
    case PixelFormat::IA4:
    case PixelFormat::I4:
    case PixelFormat::A4:
    case PixelFormat::ETC1:
    case PixelFormat::ETC1A4:
        return {DkImageFormat_RGBA8_Unorm, 4, false};
    case PixelFormat::D16:
        return {DkImageFormat_Z16, 2, false};
    case PixelFormat::D24:
        return {DkImageFormat_Z24X8, 4, false};
    case PixelFormat::D24S8:
        return {DkImageFormat_Z24S8, 4, false};
    default:
        return {};
    }
}

u32 LevelWidth(const Surface& surface, u32 level) {
    return std::max(1U, surface.GetScaledWidth() >> level);
}

u32 LevelHeight(const Surface& surface, u32 level) {
    return std::max(1U, surface.GetScaledHeight() >> level);
}

std::size_t LevelOffsetBytes(const Surface& surface, u32 level) {
    const u32 bpp = surface.GetInternalBytesPerPixel();
    std::size_t offset = 0;
    for (u32 current = 0; current < level && current < surface.levels; ++current) {
        offset += static_cast<std::size_t>(LevelWidth(surface, current)) *
                  LevelHeight(surface, current) * bpp;
    }
    return offset;
}

#ifdef __SWITCH__
bool IsTraceRuntimeFormat(VideoCore::PixelFormat format) {
    using VideoCore::PixelFormat;
    switch (format) {
    case PixelFormat::IA8:
    case PixelFormat::RG8:
    case PixelFormat::I8:
    case PixelFormat::A8:
    case PixelFormat::IA4:
    case PixelFormat::I4:
    case PixelFormat::A4:
    case PixelFormat::ETC1:
    case PixelFormat::ETC1A4:
        return true;
    default:
        return false;
    }
}

bool ShouldTraceRuntimeSurface(const Surface& surface) {
    if (!IsTraceRuntimeFormat(surface.pixel_format)) {
        return false;
    }
    static std::atomic_uint trace_count{0};
    return trace_count.fetch_add(1, std::memory_order_relaxed) < 512;
}

bool ShouldTraceRuntimeFailure() {
    static std::atomic_uint trace_count{0};
    return trace_count.fetch_add(1, std::memory_order_relaxed) < 128;
}

void LogRuntimeSurface(const char* step, const Surface& surface, int line, u32 level = 0,
                       u32 byte_size = 0, u32 x = 0, u32 y = 0, u32 rect_width = 0,
                       u32 rect_height = 0) {
    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=deko3d.stack step=%s func=Deko3D::Surface line=%d "
        "addr=%08X size=%ux%u levels=%u format=%u bpp=%u level=%u bytes=%u "
        "rect=%u,%u,%ux%u",
        step, line, static_cast<u32>(surface.addr), surface.width, surface.height,
        surface.levels, static_cast<u32>(surface.pixel_format), surface.GetInternalBytesPerPixel(),
        level, byte_size, x, y, rect_width, rect_height);
}
#endif

} // namespace

MemBlock::~MemBlock() {
    Destroy();
}

MemBlock::MemBlock(MemBlock&& other) noexcept
    : block{std::exchange(other.block, {})}, size{std::exchange(other.size, 0)} {}

MemBlock& MemBlock::operator=(MemBlock&& other) noexcept {
    if (this != &other) {
        Destroy();
        block = std::exchange(other.block, {});
        size = std::exchange(other.size, 0);
    }
    return *this;
}

bool MemBlock::Create(dk::Device device, u32 requested_size, u32 flags) {
    Destroy();
    if (!device) {
        return false;
    }
    size = AlignUp(std::max(requested_size, static_cast<u32>(DK_MEMBLOCK_ALIGNMENT)),
                   static_cast<u32>(DK_MEMBLOCK_ALIGNMENT));
    block = dk::MemBlockMaker{device, size}.setFlags(flags).create();
    return static_cast<bool>(block);
}

void MemBlock::Destroy() {
    if (block) {
        block.destroy();
    }
    block = {};
    size = 0;
}

TextureRuntime::TextureRuntime(dk::Device device_, dk::Queue queue_, dk::CmdBuf command_buffer_) {
    SetContext(device_, queue_, command_buffer_);
}

TextureRuntime::~TextureRuntime() = default;

void TextureRuntime::SetContext(dk::Device device_, dk::Queue queue_, dk::CmdBuf command_buffer_) {
    device = device_;
    queue = queue_;
    command_buffer = command_buffer_;
}

u32 TextureRuntime::RemoveThreshold() {
    return 3;
}

void TextureRuntime::Finish() {
    if (queue) {
        queue.waitIdle();
    }
}

VideoCore::StagingData TextureRuntime::FindStaging(u32 size, bool upload) {
    if (staging_buffer.size() < size) {
        staging_buffer.resize(size);
    }
    return {
        .size = size,
        .offset = 0,
        .mapped = std::span<u8>{staging_buffer.data(), size},
    };
}

bool TextureRuntime::Reinterpret(Surface& source, Surface& dest,
                                 const VideoCore::TextureCopy& copy) {
    if (source.GetInternalBytesPerPixel() != dest.GetInternalBytesPerPixel()) {
        return false;
    }
    return CopyTextures(source, dest, copy);
}

bool TextureRuntime::ClearTexture(Surface& surface, const VideoCore::TextureClear& clear) {
    surface.AllocateStorage();
    const u32 bpp = surface.GetInternalBytesPerPixel();
    if (bpp == 0 || surface.pixels.empty()) {
        return false;
    }

    const std::size_t level_offset = LevelOffsetBytes(surface, clear.texture_level);
    const u32 left = RectLeft(clear.texture_rect);
    const u32 right = std::min(RectRight(clear.texture_rect), LevelWidth(surface, clear.texture_level));
    const u32 top = RectTop(clear.texture_rect);
    const u32 bottom =
        std::min(RectBottom(clear.texture_rect), LevelHeight(surface, clear.texture_level));
    if (left >= right || top >= bottom) {
        return true;
    }

    std::array<u8, 4> color{FloatToByte(clear.value.color.x), FloatToByte(clear.value.color.y),
                            FloatToByte(clear.value.color.z), FloatToByte(clear.value.color.w)};
    for (u32 y = top; y < bottom; ++y) {
        for (u32 x = left; x < right; ++x) {
            u8* dst = surface.pixels.data() + level_offset +
                      (static_cast<std::size_t>(y) * LevelWidth(surface, clear.texture_level) +
                                               x) *
                                                  bpp;
            std::memcpy(dst, color.data(), std::min<u32>(bpp, color.size()));
        }
    }
    surface.image_dirty = true;
    surface.UploadImageIfNeeded();
    return true;
}

bool TextureRuntime::CopyTextures(Surface& source, Surface& dest,
                                  std::span<const VideoCore::TextureCopy> copies) {
    source.AllocateStorage();
    dest.AllocateStorage();
    const u32 bpp = source.GetInternalBytesPerPixel();
    if (bpp == 0 || bpp != dest.GetInternalBytesPerPixel()) {
        return false;
    }

    for (const auto& copy : copies) {
        const u32 src_width = LevelWidth(source, copy.src_level);
        const u32 src_height = LevelHeight(source, copy.src_level);
        const u32 dst_width = LevelWidth(dest, copy.dst_level);
        const u32 dst_height = LevelHeight(dest, copy.dst_level);
        if (copy.src_offset.x >= src_width || copy.src_offset.y >= src_height ||
            copy.dst_offset.x >= dst_width || copy.dst_offset.y >= dst_height) {
            continue;
        }
        const u32 width = std::min(copy.extent.width, src_width - copy.src_offset.x);
        const u32 height =
            std::min(copy.extent.height, src_height - copy.src_offset.y);
        const u32 clipped_width = std::min(width, dst_width - copy.dst_offset.x);
        const u32 clipped_height = std::min(height, dst_height - copy.dst_offset.y);
        const std::size_t src_level_offset = LevelOffsetBytes(source, copy.src_level);
        const std::size_t dst_level_offset = LevelOffsetBytes(dest, copy.dst_level);
        for (u32 y = 0; y < clipped_height; ++y) {
            const u8* src = source.pixels.data() +
                            src_level_offset +
                            (static_cast<std::size_t>(copy.src_offset.y + y) * src_width +
                             copy.src_offset.x) *
                                bpp;
            u8* dst = dest.pixels.data() + dst_level_offset +
                      (static_cast<std::size_t>(copy.dst_offset.y + y) * dst_width +
                       copy.dst_offset.x) *
                          bpp;
            std::memcpy(dst, src, static_cast<std::size_t>(clipped_width) * bpp);
        }
    }
    dest.image_dirty = true;
    dest.UploadImageIfNeeded();
    return true;
}

bool TextureRuntime::BlitTextures(Surface& source, Surface& dest,
                                  const VideoCore::TextureBlit& blit) {
    source.AllocateStorage();
    dest.AllocateStorage();
    const u32 bpp = source.GetInternalBytesPerPixel();
    if (bpp == 0 || bpp != dest.GetInternalBytesPerPixel()) {
        return false;
    }

    const u32 src_left = RectLeft(blit.src_rect);
    const u32 src_right = std::min(RectRight(blit.src_rect), LevelWidth(source, blit.src_level));
    const u32 src_top = RectTop(blit.src_rect);
    const u32 src_bottom =
        std::min(RectBottom(blit.src_rect), LevelHeight(source, blit.src_level));
    const u32 dst_left = RectLeft(blit.dst_rect);
    const u32 dst_right = std::min(RectRight(blit.dst_rect), LevelWidth(dest, blit.dst_level));
    const u32 dst_top = RectTop(blit.dst_rect);
    const u32 dst_bottom = std::min(RectBottom(blit.dst_rect), LevelHeight(dest, blit.dst_level));
    if (src_left >= src_right || src_top >= src_bottom || dst_left >= dst_right ||
        dst_top >= dst_bottom) {
        return true;
    }
    const u32 src_width = src_right - src_left;
    const u32 src_height = src_bottom - src_top;
    const u32 dst_width = dst_right - dst_left;
    const u32 dst_height = dst_bottom - dst_top;
    if (src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0) {
        return true;
    }

    const std::size_t src_level_offset = LevelOffsetBytes(source, blit.src_level);
    const std::size_t dst_level_offset = LevelOffsetBytes(dest, blit.dst_level);
    const u32 src_level_width = LevelWidth(source, blit.src_level);
    const u32 dst_level_width = LevelWidth(dest, blit.dst_level);
    for (u32 y = 0; y < dst_height; ++y) {
        const u32 src_y = src_top + (static_cast<u64>(y) * src_height) / dst_height;
        for (u32 x = 0; x < dst_width; ++x) {
            const u32 src_x = src_left + (static_cast<u64>(x) * src_width) / dst_width;
            const u8* src = source.pixels.data() +
                            src_level_offset +
                            (static_cast<std::size_t>(src_y) * src_level_width + src_x) *
                                bpp;
            u8* dst = dest.pixels.data() + dst_level_offset +
                      (static_cast<std::size_t>(dst_top + y) * dst_level_width +
                       (dst_left + x)) *
                          bpp;
            std::memcpy(dst, src, bpp);
        }
    }
    dest.image_dirty = true;
    dest.UploadImageIfNeeded();
    return true;
}

void TextureRuntime::GenerateMipmaps(Surface& surface) {
    surface.UploadImageIfNeeded();
}

bool TextureRuntime::NeedsConversion(const Surface& surface) const {
    return GetFormatInfo(surface.pixel_format).needs_conversion;
}

bool TextureRuntime::HasContext() const {
    return device && queue && command_buffer;
}

Surface::Surface(TextureRuntime& runtime_, const VideoCore::SurfaceParams& params,
                 const VideoCore::SurfaceFlagBits& initial_flag_bits)
    : SurfaceBase{params, initial_flag_bits}, runtime{&runtime_} {
    AllocateStorage();
}

Surface::Surface(TextureRuntime& runtime_, const VideoCore::SurfaceBase& surface,
                 const VideoCore::Material* material_)
    : SurfaceBase{static_cast<const VideoCore::SurfaceParams&>(surface), surface.flags},
      runtime{&runtime_} {
    material = material_;
    invalid_regions = surface.invalid_regions;
    fill_size = surface.fill_size;
    fill_data = surface.fill_data;
    modification_tick = surface.modification_tick;
    AllocateStorage();
}

Surface::~Surface() = default;

Surface::Surface(Surface&& other) noexcept
    : SurfaceBase{static_cast<const VideoCore::SurfaceParams&>(other), other.flags},
      runtime{std::exchange(other.runtime, nullptr)}, image{std::exchange(other.image, {})},
      image_memory{std::move(other.image_memory)},
      upload_retire_blocks{std::move(other.upload_retire_blocks)},
      pixels{std::move(other.pixels)}, image_ready{std::exchange(other.image_ready, false)},
      image_dirty{std::exchange(other.image_dirty, true)} {
    material = other.material;
    invalid_regions = std::move(other.invalid_regions);
    fill_size = other.fill_size;
    fill_data = other.fill_data;
    modification_tick = other.modification_tick;
}

Surface& Surface::operator=(Surface&& other) noexcept {
    if (this != &other) {
        VideoCore::SurfaceParams::operator=(static_cast<const VideoCore::SurfaceParams&>(other));
        flags = other.flags;
        material = other.material;
        invalid_regions = std::move(other.invalid_regions);
        fill_size = other.fill_size;
        fill_data = other.fill_data;
        modification_tick = other.modification_tick;
        runtime = std::exchange(other.runtime, nullptr);
        image = std::exchange(other.image, {});
        image_memory = std::move(other.image_memory);
        upload_retire_blocks = std::move(other.upload_retire_blocks);
        pixels = std::move(other.pixels);
        image_ready = std::exchange(other.image_ready, false);
        image_dirty = std::exchange(other.image_dirty, true);
    }
    return *this;
}

dk::ImageView Surface::ImageView(u32 level) {
    UploadImageIfNeeded();
    dk::ImageView view{image};
    if (image_ready && levels != 0) {
        const u32 first_level = std::min(level, levels - 1);
        view.setMipLevels(static_cast<u8>(first_level),
                          static_cast<u8>(std::max(1U, levels - first_level)));
    }
    return view;
}

void Surface::Upload(const VideoCore::BufferTextureCopy& upload,
                     const VideoCore::StagingData& staging) {
    AllocateStorage();
#ifdef __SWITCH__
    const bool trace_upload = ShouldTraceRuntimeSurface(*this);
    if (trace_upload) {
        LogRuntimeSurface("surface-upload.begin", *this, __LINE__, upload.texture_level,
                          staging.size);
    }
#endif
    const u32 bpp = GetInternalBytesPerPixel();
    const std::size_t level_offset = LevelOffsetBytes(*this, upload.texture_level);
    const u32 level_width = LevelWidth(*this, upload.texture_level);
    const u32 level_height = LevelHeight(*this, upload.texture_level);
    const u32 left = RectLeft(upload.texture_rect);
    const u32 right = std::min(RectRight(upload.texture_rect), level_width);
    const u32 top = RectTop(upload.texture_rect);
    const u32 bottom = std::min(RectBottom(upload.texture_rect), level_height);
    if (left >= right || top >= bottom) {
        return;
    }
    const u32 width = right - left;
    const u32 height = bottom - top;
    const std::size_t row_size = static_cast<std::size_t>(width) * bpp;
    if (width == 0 || height == 0 || staging.mapped.size() < row_size * height) {
        return;
    }
#ifdef __SWITCH__
    if (trace_upload) {
        LogRuntimeSurface("surface-upload.rect", *this, __LINE__, upload.texture_level,
                          staging.size, left, top, width, height);
    }
#endif

    for (u32 y = 0; y < height; ++y) {
        const u8* src = staging.mapped.data() + y * row_size;
        u8* dst = pixels.data() + level_offset +
                  (static_cast<std::size_t>(top + y) * level_width + left) * bpp;
        std::memcpy(dst, src, row_size);
    }
    image_dirty = true;
    if (UploadImageRegion(upload.texture_level, left, top, width, height, staging.mapped.data(),
                          static_cast<u32>(row_size * height))) {
        image_dirty = false;
    }
#ifdef __SWITCH__
    if (trace_upload) {
        LogRuntimeSurface("surface-upload.end", *this, __LINE__, upload.texture_level,
                          staging.size, left, top, width, height);
    }
#endif
}

void Surface::UploadCustom(const VideoCore::Material* material_, u32 level) {
    material = material_;
    AllocateStorage();
    if (material != nullptr) {
        const auto* texture = material->Map(VideoCore::MapType::Color);
        if (texture != nullptr && texture->IsLoaded()) {
            const std::size_t offset = LevelOffsetBytes(*this, level);
            if (offset < pixels.size()) {
                const std::size_t copy_size =
                    std::min<std::size_t>(texture->data.size(), pixels.size() - offset);
                std::memcpy(pixels.data() + offset, texture->data.data(), copy_size);
            }
        }
    }
    image_dirty = true;
    UploadImageIfNeeded();
}

void Surface::Download(const VideoCore::BufferTextureCopy& download,
                       const VideoCore::StagingData& staging) {
    AllocateStorage();
    const u32 bpp = GetInternalBytesPerPixel();
    const std::size_t level_offset = LevelOffsetBytes(*this, download.texture_level);
    const u32 level_width = LevelWidth(*this, download.texture_level);
    const u32 level_height = LevelHeight(*this, download.texture_level);
    const u32 left = RectLeft(download.texture_rect);
    const u32 right = std::min(RectRight(download.texture_rect), level_width);
    const u32 top = RectTop(download.texture_rect);
    const u32 bottom = std::min(RectBottom(download.texture_rect), level_height);
    if (left >= right || top >= bottom) {
        return;
    }
    const u32 width = right - left;
    const u32 height = bottom - top;
    const std::size_t row_size = static_cast<std::size_t>(width) * bpp;
    if (width == 0 || height == 0 || staging.mapped.size() < row_size * height) {
        return;
    }

    for (u32 y = 0; y < height; ++y) {
        const u8* src = pixels.data() + level_offset +
                        (static_cast<std::size_t>(top + y) * level_width + left) * bpp;
        u8* dst = staging.mapped.data() + y * row_size;
        std::memcpy(dst, src, row_size);
    }
}

void Surface::ScaleUp(u32 new_scale) {
    if (new_scale <= res_scale) {
        return;
    }
    res_scale = new_scale;
    AllocateStorage();
    image_ready = false;
    image_dirty = true;
}

u32 Surface::GetInternalBytesPerPixel() const {
    if (type == VideoCore::SurfaceType::Fill || pixel_format == VideoCore::PixelFormat::Invalid) {
        return 0;
    }
    return GetFormatInfo(pixel_format).bytes_per_pixel;
}

void Surface::AllocateStorage() {
    if (type == VideoCore::SurfaceType::Fill || pixel_format == VideoCore::PixelFormat::Invalid) {
        pixels.clear();
        image_dirty = false;
        return;
    }
    std::size_t size = 0;
    const u32 bpp = GetInternalBytesPerPixel();
    for (u32 level = 0; level < levels; ++level) {
        size += static_cast<std::size_t>(LevelWidth(*this, level)) * LevelHeight(*this, level) * bpp;
    }
    if (pixels.size() != size) {
        pixels.assign(size, 0);
        image_dirty = true;
    }
}

bool Surface::EnsureImage() {
    if (image_ready) {
        return true;
    }
    const FormatInfo format_info = GetFormatInfo(pixel_format);
#ifdef __SWITCH__
    const bool trace_failure = ShouldTraceRuntimeSurface(*this) || ShouldTraceRuntimeFailure();
#endif
    if (runtime == nullptr || !runtime->HasContext() ||
        format_info.format == DkImageFormat_None) {
#ifdef __SWITCH__
        if (trace_failure) {
            LogRuntimeSurface("gpu-image.ensure.fail-context", *this, __LINE__, 0,
                              static_cast<u32>(format_info.format));
        }
#endif
        return false;
    }

    dk::ImageLayout layout;
    dk::ImageLayoutMaker{runtime->device}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
        .setFormat(format_info.format)
        .setDimensions(GetScaledWidth(), GetScaledHeight())
        .setMipLevels(levels)
        .initialize(layout);

    if (!image_memory.Create(runtime->device, layout.getSize(),
                             DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                 DkMemBlockFlags_Image)) {
#ifdef __SWITCH__
        if (trace_failure) {
            LogRuntimeSurface("gpu-image.memory.fail", *this, __LINE__, 0,
                              static_cast<u32>(layout.getSize()));
        }
#endif
        return false;
    }
    image.initialize(layout, image_memory.block, 0);
    image_ready = true;
    image_dirty = true;
    return true;
}

MemBlock* Surface::AllocateUploadBlock(u32 byte_size) {
    if (runtime == nullptr || !runtime->device || byte_size == 0) {
        return nullptr;
    }

    upload_retire_blocks.emplace_back();
    MemBlock& upload = upload_retire_blocks.back();
    if (!upload.Create(runtime->device, byte_size,
                       DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)) {
        upload_retire_blocks.pop_back();
        return nullptr;
    }
    return &upload;
}

bool Surface::UploadImageRegion(u32 level, u32 x, u32 y, u32 width, u32 height, const u8* src,
                                u32 byte_size) {
    if (width == 0 || height == 0 || src == nullptr || byte_size == 0) {
        return true;
    }
#ifdef __SWITCH__
    const bool trace_upload = ShouldTraceRuntimeSurface(*this);
    if (trace_upload) {
        LogRuntimeSurface("gpu-upload.region.begin", *this, __LINE__, level, byte_size, x, y,
                          width, height);
    }
#endif
    if (!EnsureImage()) {
#ifdef __SWITCH__
        if (trace_upload || ShouldTraceRuntimeFailure()) {
            LogRuntimeSurface("gpu-upload.region.ensure.fail", *this, __LINE__, level, byte_size,
                              x, y, width, height);
        }
#endif
        return false;
    }
    MemBlock* upload_memory = AllocateUploadBlock(byte_size);
    if (upload_memory == nullptr) {
#ifdef __SWITCH__
        if (trace_upload || ShouldTraceRuntimeFailure()) {
            LogRuntimeSurface("gpu-upload.region.memory.fail", *this, __LINE__, level,
                              byte_size, x, y, width, height);
        }
#endif
        return false;
    }

    auto* dst = static_cast<u8*>(upload_memory->block.getCpuAddr());
    std::memcpy(dst, src, byte_size);
    upload_memory->block.flushCpuCache(0, byte_size);

    dk::ImageView image_view{image};
    image_view.setMipLevels(static_cast<u8>(std::min(level, levels - 1)), 1);
    // Keep region uploads tightly packed like the full-image upload path.
    // Ryujinx's Vulkan backend has hit zero-sized host-copy failures with
    // explicit rowLength/imageHeight on tiny converted I4/A4 regions.
    const DkCopyBuf upload_buffer{upload_memory->block.getGpuAddr(), 0, 0};
    const DkImageRect image_rect{x, y, 0, width, height, 1};
#ifdef __SWITCH__
    if (trace_upload) {
        LogRuntimeSurface("gpu-upload.region.copy.begin", *this, __LINE__, level, byte_size, x, y,
                          width, height);
    }
#endif
    runtime->command_buffer.copyBufferToImage(upload_buffer, image_view, image_rect);
#ifdef __SWITCH__
    if (trace_upload) {
        LogRuntimeSurface("gpu-upload.region.copy.end", *this, __LINE__, level, byte_size, x, y,
                          width, height);
    }
#endif
    runtime->command_buffer.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
#ifdef __SWITCH__
    if (trace_upload) {
        LogRuntimeSurface("gpu-upload.region.end", *this, __LINE__, level, byte_size, x, y, width,
                          height);
    }
#endif
    return true;
}

void Surface::UploadImageIfNeeded() {
    if (!image_dirty) {
        return;
    }
#ifdef __SWITCH__
    const bool trace_upload = ShouldTraceRuntimeSurface(*this);
    if (trace_upload) {
        LogRuntimeSurface("gpu-upload.ensure.begin", *this, __LINE__, 0,
                          static_cast<u32>(pixels.size()));
    }
#endif
    if (!EnsureImage()) {
#ifdef __SWITCH__
        if (trace_upload) {
            LogRuntimeSurface("gpu-upload.ensure.fail", *this, __LINE__, 0,
                              static_cast<u32>(pixels.size()));
        }
#endif
        return;
    }
    const u32 byte_size = static_cast<u32>(pixels.size());
#ifdef __SWITCH__
    if (trace_upload) {
        LogRuntimeSurface("gpu-upload.begin", *this, __LINE__, 0, byte_size);
    }
#endif
    MemBlock* upload_memory = AllocateUploadBlock(byte_size);
    if (upload_memory == nullptr) {
#ifdef __SWITCH__
        if (trace_upload || ShouldTraceRuntimeFailure()) {
            LogRuntimeSurface("gpu-upload.memory.fail", *this, __LINE__, 0, byte_size);
        }
#endif
        return;
    }
    auto* dst = static_cast<u8*>(upload_memory->block.getCpuAddr());
    std::memcpy(dst, pixels.data(), pixels.size());
    upload_memory->block.flushCpuCache(0, byte_size);

    for (u32 level = 0; level < levels; ++level) {
        const std::size_t level_offset = LevelOffsetBytes(*this, level);
        const DkCopyBuf upload_buffer{
            upload_memory->block.getGpuAddr() + static_cast<DkGpuAddr>(level_offset), 0, 0};
        dk::ImageView image_view{image};
        image_view.setMipLevels(static_cast<u8>(level), 1);
        const DkImageRect image_rect{0, 0, 0, LevelWidth(*this, level),
                                     LevelHeight(*this, level), 1};
#ifdef __SWITCH__
        if (trace_upload) {
            LogRuntimeSurface("gpu-upload.level", *this, __LINE__, level,
                              static_cast<u32>(LevelWidth(*this, level) *
                                               LevelHeight(*this, level) *
                                               GetInternalBytesPerPixel()),
                              0, 0, LevelWidth(*this, level), LevelHeight(*this, level));
            LogRuntimeSurface("gpu-upload.level.copy.begin", *this, __LINE__, level,
                              static_cast<u32>(LevelWidth(*this, level) *
                                               LevelHeight(*this, level) *
                                               GetInternalBytesPerPixel()),
                              0, 0, LevelWidth(*this, level), LevelHeight(*this, level));
        }
#endif
        runtime->command_buffer.copyBufferToImage(upload_buffer, image_view, image_rect);
#ifdef __SWITCH__
        if (trace_upload) {
            LogRuntimeSurface("gpu-upload.level.copy.end", *this, __LINE__, level,
                              static_cast<u32>(LevelWidth(*this, level) *
                                               LevelHeight(*this, level) *
                                               GetInternalBytesPerPixel()),
                              0, 0, LevelWidth(*this, level), LevelHeight(*this, level));
        }
#endif
    }
    runtime->command_buffer.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    image_dirty = false;
#ifdef __SWITCH__
    if (trace_upload) {
        LogRuntimeSurface("gpu-upload.end", *this, __LINE__, 0, byte_size);
    }
#endif
}

Framebuffer::Framebuffer(TextureRuntime& runtime, const VideoCore::FramebufferParams& params,
                         Surface* color_, Surface* depth_stencil_)
    : VideoCore::FramebufferParams{params}, color{color_}, depth_stencil{depth_stencil_} {
    if (color != nullptr) {
        res_scale = color->res_scale;
    } else if (depth_stencil != nullptr) {
        res_scale = depth_stencil->res_scale;
    }
}

Framebuffer::~Framebuffer() = default;

Sampler::Sampler(TextureRuntime& runtime, VideoCore::SamplerParams params) {
    (void)runtime;
    sampler.setFilter(PicaToDeko::TextureFilterMode(params.min_filter),
                      PicaToDeko::TextureFilterMode(params.mag_filter),
                      PicaToDeko::TextureMipFilterMode(params.mip_filter));
    sampler.setWrapMode(PicaToDeko::WrapMode(params.wrap_s), PicaToDeko::WrapMode(params.wrap_t),
                        DkWrapMode_ClampToEdge);
    sampler.setLodClamp(static_cast<float>(params.lod_min), static_cast<float>(params.lod_max));
    constexpr float inv_255 = 1.0f / 255.0f;
    sampler.setBorderColor(static_cast<float>(params.border_color & 0xff) * inv_255,
                           static_cast<float>((params.border_color >> 8) & 0xff) * inv_255,
                           static_cast<float>((params.border_color >> 16) & 0xff) * inv_255,
                           static_cast<float>((params.border_color >> 24) & 0xff) * inv_255);
}

Sampler::~Sampler() = default;

DebugScope::DebugScope(TextureRuntime& runtime, Common::Vec4f color, std::string_view label) {}

DebugScope::~DebugScope() = default;

} // namespace Deko3D
