// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_deko3d/deko_scheduler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_deko3d/deko_instance.h"
#include "video_core/renderer_deko3d/deko_master_fence.h"
#include "video_core/renderer_deko3d/deko_resource_pool.h"

namespace Deko3D {

Scheduler::Scheduler(Instance& instance_) : instance{instance_} {
    master_fence = std::make_unique<MasterFence>(instance);
    cmd_pool = std::make_unique<CommandPool>(instance, master_fence.get());
    AcquireNextCommandBuffer();
}

Scheduler::~Scheduler() {
    // CurrentTick() is the tick of the *next* (unsubmitted) submission. Subtract
    // one to wait for the last actually-submitted command list. If nothing has
    // been submitted (current_tick is still its initial value of 1), there is
    // nothing to wait on. Skipping this check used to deadlock the destructor
    // on a tick the GPU could never reach.
    const u64 last_submitted = master_fence->CurrentTick();
    if (last_submitted > 1) {
        master_fence->Wait(last_submitted - 1);
    }
}

u64 Scheduler::CurrentTick() const noexcept {
    return master_fence->CurrentTick();
}

u64 Scheduler::Flush() {
    const DkCmdList list = active_cmdbuf.finishList();
    const u64 tick = master_fence->NextTick();
    master_fence->SubmitWork(instance.GetQueue(), list, tick);
    AcquireNextCommandBuffer();
    return tick;
}

void Scheduler::Finish() {
    const u64 tick = Flush();
    master_fence->Wait(tick);
}

void Scheduler::Wait(u64 tick) {
    master_fence->Wait(tick);
}

void Scheduler::AcquireNextCommandBuffer() {
    active_cmdbuf = cmd_pool->Commit();
}

} // namespace Deko3D
