// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.
//
// Scheduler — owner of the per-frame command buffer and the bridge between
// renderer code and the deko3d queue. Mirrors a stripped-down vk_scheduler:
// no worker thread (recording is synchronous), no chunked Record() API.
// Callers grab the current dk::CmdBuf via CommandBuffer(), record into it,
// then Flush() or Finish() to submit + advance the tick.

#pragma once

#include <memory>

#include <deko3d.hpp>

#include "common/common_types.h"

namespace Deko3D {

class Instance;
class MasterFence;
class CommandPool;

class Scheduler {
public:
    explicit Scheduler(Instance& instance);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// Returns the active dk::CmdBuf. Recording happens directly into it.
    [[nodiscard]] dk::CmdBuf CommandBuffer() const noexcept {
        return active_cmdbuf;
    }

    /// Tick that will be associated with the next submission. Useful for
    /// resources that want to be invalidated when the current frame finishes.
    [[nodiscard]] u64 CurrentTick() const noexcept;

    /// Finalise the current cmdbuf and submit it to the queue. Returns the
    /// tick at which the GPU will report completion.
    u64 Flush();

    /// Flush + block until the GPU finishes the submitted work.
    void Finish();

    /// Block until the GPU has executed up to and including `tick`.
    void Wait(u64 tick);

    /// Direct access for components that want to poll the master fence.
    [[nodiscard]] MasterFence& Fence() noexcept {
        return *master_fence;
    }

private:
    void AcquireNextCommandBuffer();

    Instance& instance;
    std::unique_ptr<MasterFence> master_fence;
    std::unique_ptr<CommandPool> cmd_pool;
    dk::CmdBuf active_cmdbuf{};
};

} // namespace Deko3D
