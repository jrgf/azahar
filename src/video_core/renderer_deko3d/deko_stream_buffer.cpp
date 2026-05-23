// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_deko3d/deko_stream_buffer.h"

#include <algorithm>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_deko3d/deko_instance.h"
#include "video_core/renderer_deko3d/deko_master_fence.h"

namespace Deko3D {

StreamBuffer::StreamBuffer(Instance& instance_, MasterFence& master_fence_, u32 size_bytes,
                           u32 mem_flags)
    : instance{instance_}, master_fence{master_fence_}, capacity{size_bytes} {
    block = dk::MemBlockMaker{instance.GetDevice(), capacity}.setFlags(mem_flags).create();
    cpu_base = static_cast<u8*>(block.getCpuAddr());
    gpu_base = block.getGpuAddr();
}

StreamBuffer::~StreamBuffer() = default;

StreamBuffer::Mapping StreamBuffer::Map(u32 size, u32 alignment) {
    ASSERT_MSG(size <= capacity, "StreamBuffer::Map requested {} > capacity {}", size, capacity);

    const u32 aligned_head = Common::AlignUp(head, alignment);
    u32 end = aligned_head + size;

    if (end > capacity) {
        // Wrap to the start of the ring; reclaim from offset 0.
        head = 0;
        Reclaim(size);
        end = head + size;
    } else {
        Reclaim(end);
    }

    mapped_offset = (end > capacity) ? 0 : Common::AlignUp(head, alignment);
    mapped_size = size;
    return Mapping{
        .cpu = cpu_base + mapped_offset,
        .gpu = gpu_base + mapped_offset,
        .offset = mapped_offset,
    };
}

void StreamBuffer::Commit(u32 bytes) {
    ASSERT(bytes <= mapped_size);
    block.flushCpuCache(mapped_offset, bytes);
    head = mapped_offset + bytes;
    watermarks.push_back(Watermark{.offset = head, .tick = master_fence.CurrentTick()});
}

void StreamBuffer::Reclaim(u32 needed_offset) {
    // Drop watermarks whose end-offset lies before our new head: those bytes
    // are already past the cursor and don't need to be retired.
    while (!watermarks.empty() && watermarks.front().offset <= head) {
        watermarks.erase(watermarks.begin());
    }
    // Block until the GPU has finished writing the regions that overlap with
    // [head, needed_offset) — only the oldest watermark in that range matters.
    while (!watermarks.empty() && watermarks.front().offset < needed_offset) {
        master_fence.Wait(watermarks.front().tick);
        watermarks.erase(watermarks.begin());
    }
}

} // namespace Deko3D
