// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_deko3d/deko_resource_pool.h"

#include <optional>

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_deko3d/deko_instance.h"
#include "video_core/renderer_deko3d/deko_master_fence.h"

namespace Deko3D {

namespace {
// One command buffer per in-flight frame, mirroring the Vulkan default. Grows
// on demand if SwapBuffers gets ahead of GPU completion.
constexpr std::size_t kCommandPoolGrowStep = 4;

// Bytes of recording memory reserved per command buffer. Sized to match the
// monolithic command-memory block the MVP renderer allocates today.
constexpr u32 kPerCmdMemorySize = 1u << 20;

// How many descriptor slots to allocate at once when the heap overflows.
constexpr std::size_t kDescriptorSetBatch = 64;
} // namespace

ResourcePool::ResourcePool(MasterFence* master_fence_, std::size_t grow_step_)
    : master_fence{master_fence_}, grow_step{grow_step_} {}

std::size_t ResourcePool::CommitResource() {
    u64 gpu_tick = master_fence->KnownGpuTick();
    const auto search = [this, gpu_tick](std::size_t begin,
                                          std::size_t end) -> std::optional<std::size_t> {
        for (std::size_t iterator = begin; iterator < end; ++iterator) {
            if (gpu_tick >= ticks[iterator]) {
                ticks[iterator] = master_fence->CurrentTick();
                return iterator;
            }
        }
        return std::nullopt;
    };

    auto found = search(hint_iterator, ticks.size());
    if (!found) {
        master_fence->Refresh();
        gpu_tick = master_fence->KnownGpuTick();
        found = search(hint_iterator, ticks.size());
    }
    if (!found) {
        found = search(0, hint_iterator);
        if (!found) {
            const std::size_t free_resource = ManageOverflow();
            ticks[free_resource] = master_fence->CurrentTick();
            found = free_resource;
        }
    }

    hint_iterator = (*found + 1) % ticks.size();
    return *found;
}

std::size_t ResourcePool::ManageOverflow() {
    const std::size_t old_capacity = ticks.size();
    ticks.resize(old_capacity + grow_step);
    Allocate(old_capacity, old_capacity + grow_step);
    return old_capacity;
}

CommandPool::CommandPool(Instance& instance_, MasterFence* master_fence)
    : ResourcePool{master_fence, kCommandPoolGrowStep}, instance{instance_} {}

CommandPool::~CommandPool() = default;

void CommandPool::Allocate(std::size_t begin, std::size_t end) {
    cmd_bufs.resize(end);
    cmd_memory.resize(end);

    const dk::Device device = instance.GetDevice();
    for (std::size_t i = begin; i < end; ++i) {
        cmd_memory[i] = dk::MemBlockMaker{device, kPerCmdMemorySize}
                            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                            .create();
        cmd_bufs[i] = dk::CmdBufMaker{device}.create();
        cmd_bufs[i].addMemory(cmd_memory[i], 0, kPerCmdMemorySize);
    }
}

dk::CmdBuf CommandPool::Commit() {
    const std::size_t index = CommitResource();
    cmd_bufs[index].clear();
    return cmd_bufs[index];
}

DescriptorHeap::DescriptorHeap(Instance& instance_, MasterFence* master_fence,
                               u32 descriptor_bytes_, u32 descriptor_heap_count_)
    : ResourcePool{master_fence, kDescriptorSetBatch}, instance{instance_},
      descriptor_bytes{descriptor_bytes_}, descriptor_heap_count{descriptor_heap_count_} {
    AppendDescriptorBlock();
}

DescriptorHeap::~DescriptorHeap() = default;

void DescriptorHeap::Allocate(std::size_t begin, std::size_t end) {
    // Each Allocate() call extends the slot_addrs vector. If the existing
    // blocks have room we slice further into them; otherwise we allocate a
    // fresh memblock to back the new slot range.
    slot_addrs.resize(end);

    const std::size_t needed = end - begin;
    std::size_t produced = 0;
    while (produced < needed) {
        if (blocks.empty() ||
            slot_addrs.size() - begin - produced > descriptor_heap_count) {
            AppendDescriptorBlock();
        }
        auto& block = blocks.back();
        const DkGpuAddr base = block.getGpuAddr();
        const std::size_t already_used =
            (slot_addrs.size() == begin + produced) ? 0 : descriptor_heap_count;
        (void)already_used; // current implementation uses one block per batch; see TODO.
        for (std::size_t slot = 0; slot < descriptor_heap_count && produced < needed;
             ++slot, ++produced) {
            slot_addrs[begin + produced] =
                base + static_cast<DkGpuAddr>(slot) * descriptor_bytes;
        }
    }
}

DkGpuAddr DescriptorHeap::Commit() {
    const std::size_t index = CommitResource();
    return slot_addrs[index];
}

void DescriptorHeap::AppendDescriptorBlock() {
    const dk::Device device = instance.GetDevice();
    const u32 size = descriptor_bytes * descriptor_heap_count;
    blocks.emplace_back(
        dk::MemBlockMaker{device, size}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create());
}

} // namespace Deko3D
