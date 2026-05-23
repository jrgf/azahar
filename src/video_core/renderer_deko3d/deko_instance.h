// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.
//
// deko3d device wrapper. Mirrors a tiny subset of vk::Instance — just enough
// to give downstream code (scheduler, pipeline cache, texture runtime) a
// single owner for the device, queue, command-memory backing block, and
// format-lookup table. deko3d has only one physical device on the Switch, so
// most of the Vulkan instance's branching disappears.

#pragma once

#include <array>
#include <span>
#include <string>

#include <deko3d.hpp>

#include "common/common_types.h"
#include "video_core/pica/regs_pipeline.h"
#include "video_core/rasterizer_cache/pixel_format.h"

namespace VideoCore {
enum class CustomPixelFormat : u32;
}

namespace Deko3D {

struct FormatTraits {
    bool transfer_support = false;
    bool blit_support = false;
    bool attachment_support = false;
    bool needs_conversion = false;
    DkImageFormat native = DkImageFormat_None;

    auto operator<=>(const FormatTraits&) const = default;
};

class Instance {
public:
    /// Owns the device — creates a fresh dk::Device + dk::Queue.
    Instance();

    /// Adopts an externally-owned device and queue. Use this when integrating
    /// with code that already created its own dk::Device (the legacy
    /// renderer does this) — Switch only allows one dk::Device per process.
    Instance(dk::Device external_device, dk::Queue external_queue);

    ~Instance();

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    /// Returns the deko3d device handle.
    [[nodiscard]] dk::Device GetDevice() const noexcept {
        return device;
    }

    /// Returns the primary graphics queue.
    [[nodiscard]] dk::Queue GetQueue() const noexcept {
        return queue;
    }

    /// FormatTraits lookup by PICA color/depth pixel format.
    [[nodiscard]] const FormatTraits& GetTraits(VideoCore::PixelFormat pixel_format) const;

    /// FormatTraits lookup by custom (host-side) pixel format.
    [[nodiscard]] const FormatTraits& GetTraits(VideoCore::CustomPixelFormat pixel_format) const;

    /// FormatTraits lookup by vertex attribute format/count.
    [[nodiscard]] const FormatTraits& GetTraits(
        Pica::PipelineRegs::VertexAttributeFormat format, u32 count) const;

    [[nodiscard]] const std::array<FormatTraits, 16>& GetAllAttribTraits() const noexcept {
        return attrib_table;
    }

    /// Default alignment for uniform buffers fed into the shader stages.
    [[nodiscard]] u32 UniformMinAlignment() const noexcept {
        return DK_UNIFORM_BUF_ALIGNMENT;
    }

    /// Required alignment for read-only shader storage buffers.
    [[nodiscard]] u32 ShaderCodeAlignment() const noexcept {
        return DK_SHADER_CODE_ALIGNMENT;
    }

    /// Whether triangle-fan primitives are supported by the GPU.
    /// deko3d supports them on Maxwell (Tegra X1) — always true.
    [[nodiscard]] bool IsTriangleFanSupported() const noexcept {
        return true;
    }

    /// Identifier passed to disk caches so they can detect when underlying
    /// hardware/driver assumptions changed.
    [[nodiscard]] std::string_view GetCacheKey() const noexcept {
        return "deko3d-tegra-x1";
    }

private:
    void BuildFormatTables();

private:
    dk::UniqueDevice owned_device;
    dk::UniqueQueue owned_queue;
    dk::Device device{};
    dk::Queue queue{};

    std::array<FormatTraits, 18> color_table{}; // PixelFormat::Invalid+1 ~= 18
    std::array<FormatTraits, 16> attrib_table{};
};

} // namespace Deko3D
