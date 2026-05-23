// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.
//
// Batched descriptor writer. Mirrors vk_descriptor_update_queue, but rather
// than vkUpdateDescriptorSets we record dk::CmdBuf::pushData calls to
// stream descriptor bytes into the per-slot GPU addresses that DescriptorHeap
// hands out. The queue lets us coalesce many AddImageSampler/AddImage calls
// behind a single barrier when the frame is finalised.

#pragma once

#include <vector>

#include <deko3d.hpp>

#include "common/common_types.h"

namespace Deko3D {

class Instance;

class DescriptorUpdateQueue {
public:
    explicit DescriptorUpdateQueue(Instance& instance);
    ~DescriptorUpdateQueue();

    DescriptorUpdateQueue(const DescriptorUpdateQueue&) = delete;
    DescriptorUpdateQueue& operator=(const DescriptorUpdateQueue&) = delete;

    /// Queue an image+sampler descriptor pair at `(set_addr, binding_index)`.
    /// The image is the result of dk::ImageView combined with its sampler.
    void AddImageSampler(DkGpuAddr set_addr, u32 binding_index, dk::ImageView view,
                         dk::Sampler sampler);

    /// Queue an image-only descriptor (no sampler) — used for storage images.
    void AddImage(DkGpuAddr set_addr, u32 binding_index, dk::ImageView view);

    /// Flush queued descriptor updates into the given command buffer. Inserts
    /// an invalidate-descriptors barrier afterwards. Safe to call with no
    /// pending updates (no-op).
    void Flush(dk::CmdBuf cmdbuf);

private:
    Instance& instance;

    struct ImageDescriptorUpdate {
        DkGpuAddr addr;
        dk::ImageDescriptor desc;
    };
    struct SamplerDescriptorUpdate {
        DkGpuAddr addr;
        dk::SamplerDescriptor desc;
    };
    std::vector<ImageDescriptorUpdate> image_updates;
    std::vector<SamplerDescriptorUpdate> sampler_updates;
};

} // namespace Deko3D
