// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.
//
// Logical-tick driven GPU/CPU sync primitive. Mirrors the fence-based
// MasterSemaphoreFence from the Vulkan backend, simplified to a single class
// because deko3d has no timeline-semaphore equivalent — only DkFence.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
#include <thread>

#include <deko3d.hpp>

#include "common/common_types.h"

namespace Deko3D {

class Instance;

class MasterFence {
public:
    explicit MasterFence(Instance& instance);
    ~MasterFence();

    MasterFence(const MasterFence&) = delete;
    MasterFence& operator=(const MasterFence&) = delete;

    /// Current logical tick (next pending submission). Monotonically increasing.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    /// Last tick the GPU is known to have completed.
    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return gpu_tick.load(std::memory_order_acquire);
    }

    /// Whether the tick is already observed as complete.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    /// Reserve a new logical tick for the next submission.
    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }

    /// Poll GPU progress without blocking; updates KnownGpuTick.
    void Refresh();

    /// Block the calling thread until the given tick is complete on the GPU.
    void Wait(u64 tick);

    /// Submit work on the device queue, tagging it with `tick`. After the GPU
    /// finishes this submission, KnownGpuTick advances past `tick`.
    void SubmitWork(dk::Queue queue, DkCmdList list, u64 tick);

private:
    void WaiterThread();

    // Borrow a free fence (recycling oldest finished one when possible).
    DkFence* AcquireFence();

private:
    Instance& instance;

    std::atomic<u64> gpu_tick{0};
    std::atomic<u64> current_tick{1};

    // Pool of fence storage. Each submission grabs one; the waiter thread
    // returns it to free_queue once its tick is observed completed.
    std::deque<DkFence> fence_storage;
    std::deque<DkFence*> free_queue;
    std::queue<std::pair<DkFence*, u64>> wait_queue;

    std::mutex free_mutex;
    std::mutex wait_mutex;
    std::condition_variable free_cv;
    std::condition_variable wait_cv;

    std::atomic<bool> stop{false};
    std::thread waiter;
};

} // namespace Deko3D
