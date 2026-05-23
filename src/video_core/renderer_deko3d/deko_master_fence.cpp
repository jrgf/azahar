// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_deko3d/deko_master_fence.h"

#include "common/logging/log.h"

namespace Deko3D {

namespace {
// Pre-allocated fence slots. 256 in-flight submissions is plenty — Vulkan
// uses a similar bound. Storage is a deque so addresses are stable across
// growth.
constexpr std::size_t kInitialFenceCount = 32;
} // namespace

// NOTE: Instance is forward-declared in the header; we don't actually need its
// definition here yet because the queue handle is passed in directly. The
// reference is kept so future logging / debug hooks can reach back if needed.

MasterFence::MasterFence(Instance& instance_) : instance{instance_} {
    fence_storage.resize(kInitialFenceCount);
    for (auto& fence : fence_storage) {
        free_queue.push_back(&fence);
    }
    waiter = std::thread([this] { WaiterThread(); });
}

MasterFence::~MasterFence() {
    stop.store(true, std::memory_order_release);
    wait_cv.notify_all();
    free_cv.notify_all();
    if (waiter.joinable()) {
        waiter.join();
    }
}

void MasterFence::Refresh() {
    // Cheap polling path: try to advance gpu_tick by checking the head of the
    // wait queue without blocking. We use DkFence_Wait with a zero timeout.
    while (true) {
        DkFence* head = nullptr;
        u64 head_tick = 0;
        {
            std::lock_guard lock{wait_mutex};
            if (wait_queue.empty()) {
                return;
            }
            head = wait_queue.front().first;
            head_tick = wait_queue.front().second;
        }

        const DkResult res = dkFenceWait(head, 0);
        if (res != DkResult_Success) {
            // Head not finished yet; nothing more to harvest.
            return;
        }

        {
            std::lock_guard lock{wait_mutex};
            if (!wait_queue.empty() && wait_queue.front().first == head) {
                wait_queue.pop();
            }
        }

        gpu_tick.store(head_tick, std::memory_order_release);

        {
            std::lock_guard lock{free_mutex};
            free_queue.push_back(head);
        }
        free_cv.notify_one();
    }
}

void MasterFence::Wait(u64 tick) {
    if (IsFree(tick)) {
        return;
    }
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // Last resort: park on a condvar that the waiter thread bumps each time
    // it harvests a fence. We do NOT wait on the DkFence directly here
    // because someone else (the waiter thread) is already responsible for it.
    std::unique_lock lock{wait_mutex};
    wait_cv.wait(lock, [&] { return stop.load(std::memory_order_acquire) || IsFree(tick); });
}

void MasterFence::SubmitWork(dk::Queue queue, DkCmdList list, u64 tick) {
    DkFence* fence = AcquireFence();
    *fence = {};

    queue.submitCommands(list);
    queue.signalFence(*fence, /*flush=*/true);

    {
        std::lock_guard lock{wait_mutex};
        wait_queue.emplace(fence, tick);
    }
    wait_cv.notify_one();
}

DkFence* MasterFence::AcquireFence() {
    std::unique_lock lock{free_mutex};
    free_cv.wait(lock, [&] {
        return stop.load(std::memory_order_acquire) || !free_queue.empty();
    });

    if (free_queue.empty()) {
        // Pool exhausted (only happens during shutdown). Grow on demand.
        fence_storage.emplace_back();
        return &fence_storage.back();
    }

    DkFence* fence = free_queue.front();
    free_queue.pop_front();
    return fence;
}

void MasterFence::WaiterThread() {
    while (!stop.load(std::memory_order_acquire)) {
        DkFence* head = nullptr;
        u64 head_tick = 0;
        {
            std::unique_lock lock{wait_mutex};
            wait_cv.wait(lock, [&] {
                return stop.load(std::memory_order_acquire) || !wait_queue.empty();
            });
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            head = wait_queue.front().first;
            head_tick = wait_queue.front().second;
        }

        // Block until the GPU finishes this submission.
        dkFenceWait(head, -1);

        {
            std::lock_guard lock{wait_mutex};
            if (!wait_queue.empty() && wait_queue.front().first == head) {
                wait_queue.pop();
            }
            gpu_tick.store(head_tick, std::memory_order_release);
        }
        wait_cv.notify_all();

        {
            std::lock_guard lock{free_mutex};
            free_queue.push_back(head);
        }
        free_cv.notify_one();
    }
}

} // namespace Deko3D
