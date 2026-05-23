// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.
//
// Ring allocator backed by a single dk::MemBlock. Mirrors vk_stream_buffer:
// callers request a contiguous chunk via Map(size, alignment), copy data
// into it, then advance via Commit() so subsequent Map calls don't overlap
// with in-flight GPU work. Old regions are reclaimed when the master fence
// observes the tick that wrote them.

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <deko3d.hpp>

#include "common/common_types.h"

namespace Deko3D {

class Instance;
class MasterFence;

class StreamBuffer {
public:
    StreamBuffer(Instance& instance, MasterFence& master_fence, u32 size_bytes,
                 u32 mem_flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    ~StreamBuffer();

    StreamBuffer(const StreamBuffer&) = delete;
    StreamBuffer& operator=(const StreamBuffer&) = delete;

    /// Reserves an aligned chunk of `size` bytes. Returns a (cpu, gpu, offset)
    /// tuple — write through `cpu`, bind via `gpu` or `offset`-relative
    /// addressing. Throws via ASSERT if `size > capacity`.
    struct Mapping {
        u8* cpu = nullptr;
        DkGpuAddr gpu = 0;
        u32 offset = 0;
    };
    Mapping Map(u32 size, u32 alignment);

    /// Marks `bytes` of the most recent Map() as in-use (flushes the CPU
    /// cache for the relevant range). Must equal or be less than the size
    /// passed to Map().
    void Commit(u32 bytes);

    /// Total backing size in bytes.
    [[nodiscard]] u32 Capacity() const noexcept {
        return capacity;
    }

    /// Base GPU address of the underlying memblock. Cached at construction
    /// because dk::MemBlock::getGpuAddr is non-const.
    [[nodiscard]] DkGpuAddr GpuBase() const noexcept {
        return gpu_base;
    }

private:
    /// Reclaim a region: wait for the master fence to pass the tick that
    /// last touched it.
    void Reclaim(u32 needed_bytes);

    Instance& instance;
    MasterFence& master_fence;
    dk::UniqueMemBlock block;
    u8* cpu_base = nullptr;
    DkGpuAddr gpu_base = 0;
    u32 capacity = 0;
    u32 head = 0;       ///< Cursor for the *next* allocation.
    u32 mapped_offset = 0; ///< Offset where the current Map() returned.
    u32 mapped_size = 0;   ///< Size requested by the current Map().

    struct Watermark {
        u32 offset;
        u64 tick;
    };
    std::vector<Watermark> watermarks; ///< Track which tick wrote up to which offset.
};

} // namespace Deko3D
