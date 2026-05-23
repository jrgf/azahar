// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_deko3d/deko_descriptor_update_queue.h"

#include "video_core/renderer_deko3d/deko_instance.h"

namespace Deko3D {

DescriptorUpdateQueue::DescriptorUpdateQueue(Instance& instance_) : instance{instance_} {
    image_updates.reserve(256);
    sampler_updates.reserve(256);
}

DescriptorUpdateQueue::~DescriptorUpdateQueue() = default;

void DescriptorUpdateQueue::AddImageSampler(DkGpuAddr set_addr, u32 binding_index,
                                            dk::ImageView view, dk::Sampler sampler) {
    dk::ImageDescriptor image_desc;
    image_desc.initialize(view);
    image_updates.push_back(ImageDescriptorUpdate{
        .addr = set_addr + binding_index * sizeof(DkImageDescriptor),
        .desc = image_desc,
    });

    dk::SamplerDescriptor sampler_desc;
    sampler_desc.initialize(sampler);
    sampler_updates.push_back(SamplerDescriptorUpdate{
        .addr = set_addr + binding_index * sizeof(DkSamplerDescriptor),
        .desc = sampler_desc,
    });
}

void DescriptorUpdateQueue::AddImage(DkGpuAddr set_addr, u32 binding_index, dk::ImageView view) {
    dk::ImageDescriptor image_desc;
    image_desc.initialize(view);
    image_updates.push_back(ImageDescriptorUpdate{
        .addr = set_addr + binding_index * sizeof(DkImageDescriptor),
        .desc = image_desc,
    });
}

void DescriptorUpdateQueue::Flush(dk::CmdBuf cmdbuf) {
    if (image_updates.empty() && sampler_updates.empty()) {
        return;
    }
    for (const auto& update : image_updates) {
        cmdbuf.pushData(update.addr, &update.desc, sizeof(update.desc));
    }
    for (const auto& update : sampler_updates) {
        cmdbuf.pushData(update.addr, &update.desc, sizeof(update.desc));
    }
    cmdbuf.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
    image_updates.clear();
    sampler_updates.clear();
}

} // namespace Deko3D
