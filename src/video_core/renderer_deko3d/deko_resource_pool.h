// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.
//
// Tick-fenced pools of recyclable GPU resources. Mirrors vk_resource_pool's
// ResourcePool / CommandPool / DescriptorHeap, adapted for deko3d's command-
// buffer + descriptor-memblock model.

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <deko3d.hpp>

#include "common/common_types.h"

namespace Deko3D {

class Instance;
class MasterFence;

/// Generic pool of N indexed slots, each tagged with the tick of its last use.
/// CommitResource() returns the index of the next slot known to be free on
/// the GPU; overflow triggers Allocate() to grow the pool.
class ResourcePool {
public:
    ResourcePool() = default;
    ResourcePool(MasterFence* master_fence, std::size_t grow_step);
    virtual ~ResourcePool() = default;

    ResourcePool(ResourcePool&&) noexcept = default;
    ResourcePool& operator=(ResourcePool&&) noexcept = default;
    ResourcePool(const ResourcePool&) = default;
    ResourcePool& operator=(const ResourcePool&) = default;

protected:
    std::size_t CommitResource();

    /// Hook for subclasses to allocate slots [begin, end).
    virtual void Allocate(std::size_t begin, std::size_t end) = 0;

private:
    std::size_t ManageOverflow();

protected:
    MasterFence* master_fence{nullptr};
    std::size_t grow_step = 0;
    std::size_t hint_iterator = 0;
    std::vector<u64> ticks;
};

/// Pool of dk::CmdBuf objects backed by a shared command-memory memblock.
/// Each Commit() returns a fresh, cleared command buffer ready for recording.
class CommandPool final : public ResourcePool {
public:
    CommandPool(Instance& instance, MasterFence* master_fence);
    ~CommandPool() override;

    void Allocate(std::size_t begin, std::size_t end) override;

    /// Reserve a command buffer for this frame's recording. The returned
    /// dk::CmdBuf has been cleared and is ready for new commands.
    dk::CmdBuf Commit();

private:
    Instance& instance;
    std::vector<dk::UniqueCmdBuf> cmd_bufs;
    std::vector<dk::UniqueMemBlock> cmd_memory;
};

/// Pool of descriptor-set "slots" inside a single shared memblock. Each slot
/// is `descriptor_bytes` wide and stored back-to-back. Commit() returns the
/// GPU address of the next free slot.
class DescriptorHeap final : public ResourcePool {
public:
    DescriptorHeap(Instance& instance, MasterFence* master_fence,
                   u32 descriptor_bytes, u32 descriptor_heap_count = 1024);
    ~DescriptorHeap() override;

    void Allocate(std::size_t begin, std::size_t end) override;

    /// Reserve a descriptor-set slot. Returns its base GPU address; the
    /// caller writes into it via dk::CmdBuf::pushData and then binds via
    /// bindImage/SamplerDescriptorSet.
    DkGpuAddr Commit();

    [[nodiscard]] u32 DescriptorBytes() const noexcept {
        return descriptor_bytes;
    }

private:
    void AppendDescriptorBlock();

    Instance& instance;
    u32 descriptor_bytes;
    u32 descriptor_heap_count;
    std::vector<dk::UniqueMemBlock> blocks;
    std::vector<DkGpuAddr> slot_addrs; ///< One per slot, indexed by ResourcePool tick array.
};

} // namespace Deko3D
