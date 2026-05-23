// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_deko3d/deko_instance.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/custom_textures/custom_format.h"

namespace Deko3D {

namespace {

constexpr FormatTraits kUnsupported{};

constexpr FormatTraits MakeColor(DkImageFormat fmt) {
    return FormatTraits{.transfer_support = true,
                        .blit_support = true,
                        .attachment_support = true,
                        .needs_conversion = false,
                        .native = fmt};
}

constexpr FormatTraits MakeDepth(DkImageFormat fmt) {
    return FormatTraits{.transfer_support = true,
                        .blit_support = false,
                        .attachment_support = true,
                        .needs_conversion = false,
                        .native = fmt};
}

constexpr FormatTraits MakeAttribute(DkImageFormat fmt) {
    return FormatTraits{.transfer_support = true,
                        .blit_support = false,
                        .attachment_support = false,
                        .needs_conversion = false,
                        .native = fmt};
}

} // namespace

Instance::Instance() {
    owned_device = dk::DeviceMaker{}.create();
    ASSERT_MSG(owned_device, "deko3d device creation failed");
    device = owned_device;

    owned_queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();
    ASSERT_MSG(owned_queue, "deko3d graphics queue creation failed");
    queue = owned_queue;

    BuildFormatTables();
    LOG_INFO(Render, "deko3d instance initialised (owned)");
}

Instance::Instance(dk::Device external_device, dk::Queue external_queue)
    : device{external_device}, queue{external_queue} {
    ASSERT_MSG(device, "Instance constructed with null external device");
    ASSERT_MSG(queue, "Instance constructed with null external queue");
    BuildFormatTables();
    LOG_INFO(Render, "deko3d instance initialised (adopted)");
}

Instance::~Instance() {
    // Only wait-idle when we created the queue. Adopted queues belong to the
    // caller and may already be torn down at this point.
    if (owned_queue) {
        queue.waitIdle();
    }
}

void Instance::BuildFormatTables() {
    color_table.fill(kUnsupported);
    attrib_table.fill(kUnsupported);

    using VideoCore::PixelFormat;
    color_table[static_cast<u32>(PixelFormat::RGBA8)] = MakeColor(DkImageFormat_RGBA8_Unorm);
    // PICA RGB8 is 24-bit; deko3d has no packed 24bpp color format. Promote to
    // RGBX8 — same memory layout per-pixel except the trailing byte is
    // undefined, which the upload path patches to 0xFF.
    color_table[static_cast<u32>(PixelFormat::RGB8)] = MakeColor(DkImageFormat_RGBX8_Unorm);
    color_table[static_cast<u32>(PixelFormat::RGB8)].needs_conversion = true;
    color_table[static_cast<u32>(PixelFormat::RGB5A1)] = MakeColor(DkImageFormat_RGB5A1_Unorm);
    color_table[static_cast<u32>(PixelFormat::RGB565)] = MakeColor(DkImageFormat_RGB565_Unorm);
    color_table[static_cast<u32>(PixelFormat::RGBA4)] = MakeColor(DkImageFormat_RGBA4_Unorm);
    color_table[static_cast<u32>(PixelFormat::D16)] = MakeDepth(DkImageFormat_Z16);
    color_table[static_cast<u32>(PixelFormat::D24)] =
        MakeDepth(DkImageFormat_Z24X8);
    color_table[static_cast<u32>(PixelFormat::D24S8)] =
        MakeDepth(DkImageFormat_Z24S8);

    // Vertex attribute formats. Indexed by (format * 4 + (count - 1)).
    using Pica::PipelineRegs;
    auto set_attrib = [this](PipelineRegs::VertexAttributeFormat fmt, u32 count,
                             DkImageFormat native) {
        attrib_table[static_cast<u32>(fmt) * 4 + (count - 1)] = MakeAttribute(native);
    };

    set_attrib(PipelineRegs::VertexAttributeFormat::BYTE, 1, DkImageFormat_R8_Snorm);
    set_attrib(PipelineRegs::VertexAttributeFormat::BYTE, 2, DkImageFormat_RG8_Snorm);
    set_attrib(PipelineRegs::VertexAttributeFormat::BYTE, 3, DkImageFormat_RGBA8_Snorm); // pad to 4
    set_attrib(PipelineRegs::VertexAttributeFormat::BYTE, 4, DkImageFormat_RGBA8_Snorm);

    set_attrib(PipelineRegs::VertexAttributeFormat::UBYTE, 1, DkImageFormat_R8_Unorm);
    set_attrib(PipelineRegs::VertexAttributeFormat::UBYTE, 2, DkImageFormat_RG8_Unorm);
    set_attrib(PipelineRegs::VertexAttributeFormat::UBYTE, 3, DkImageFormat_RGBA8_Unorm);
    set_attrib(PipelineRegs::VertexAttributeFormat::UBYTE, 4, DkImageFormat_RGBA8_Unorm);

    set_attrib(PipelineRegs::VertexAttributeFormat::SHORT, 1, DkImageFormat_R16_Snorm);
    set_attrib(PipelineRegs::VertexAttributeFormat::SHORT, 2, DkImageFormat_RG16_Snorm);
    set_attrib(PipelineRegs::VertexAttributeFormat::SHORT, 3, DkImageFormat_RGBA16_Snorm);
    set_attrib(PipelineRegs::VertexAttributeFormat::SHORT, 4, DkImageFormat_RGBA16_Snorm);

    set_attrib(PipelineRegs::VertexAttributeFormat::FLOAT, 1, DkImageFormat_R32_Float);
    set_attrib(PipelineRegs::VertexAttributeFormat::FLOAT, 2, DkImageFormat_RG32_Float);
    set_attrib(PipelineRegs::VertexAttributeFormat::FLOAT, 3, DkImageFormat_RGB32_Float);
    set_attrib(PipelineRegs::VertexAttributeFormat::FLOAT, 4, DkImageFormat_RGBA32_Float);
}

const FormatTraits& Instance::GetTraits(VideoCore::PixelFormat pixel_format) const {
    const auto idx = static_cast<u32>(pixel_format);
    if (idx >= color_table.size()) {
        return kUnsupported;
    }
    return color_table[idx];
}

const FormatTraits& Instance::GetTraits(VideoCore::CustomPixelFormat pixel_format) const {
    // Custom textures fall back to RGBA8 for now; the table can be extended
    // when M3/M4 introduces them.
    static constexpr FormatTraits rgba8 =
        FormatTraits{.transfer_support = true,
                     .blit_support = true,
                     .attachment_support = false,
                     .needs_conversion = false,
                     .native = DkImageFormat_RGBA8_Unorm};
    (void)pixel_format;
    return rgba8;
}

const FormatTraits& Instance::GetTraits(Pica::PipelineRegs::VertexAttributeFormat format,
                                        u32 count) const {
    ASSERT(count >= 1 && count <= 4);
    return attrib_table[static_cast<u32>(format) * 4 + (count - 1)];
}

} // namespace Deko3D
