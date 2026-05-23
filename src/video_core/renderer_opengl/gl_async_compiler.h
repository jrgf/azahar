// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

#ifdef __SWITCH__

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <glad/glad.h>

namespace OpenGL {

// Switch-only background shader compiler. Owns one worker thread bound to a
// shared EGL context (validated by gl_async_probe). Jobs are deduplicated by
// config_hash and the queue is abandoned on shutdown — see design notes in
// the async tier-2 plan.
//
// Callbacks fire on the worker thread; consumers are responsible for any
// hand-off to the main thread (typically an atomic store into a cache entry).
class AsyncShaderCompiler {
public:
    using Callback = std::function<void(GLuint program)>;

    // Creates and starts the worker. Returns nullptr if a shared EGL context
    // can't be set up on this platform (logged under
    // stage=opengl.async.compiler.init).
    static std::unique_ptr<AsyncShaderCompiler> Create();

    AsyncShaderCompiler(const AsyncShaderCompiler&) = delete;
    AsyncShaderCompiler& operator=(const AsyncShaderCompiler&) = delete;
    ~AsyncShaderCompiler();

    // Enqueues a single-stage separable program job. Mirrors the shader
    // manager's separable usage: one stage per program, bound later via
    // glUseProgramStages. Silently dropped if `config_hash` is already in
    // flight. Callback runs on the worker thread; receives 0 on failure.
    void Enqueue(std::uint64_t config_hash, GLenum stage_type, std::string source,
                 Callback on_ready);

    // Total jobs ever enqueued (not counting dropped duplicates).
    std::uint64_t EnqueuedCount() const {
        return enqueued_count_.load(std::memory_order_relaxed);
    }
    // Jobs the worker has finished (success or failure).
    std::uint64_t CompletedCount() const {
        return completed_count_.load(std::memory_order_relaxed);
    }

private:
    struct Job;

    AsyncShaderCompiler(void* display, void* shared_ctx);
    void WorkerLoop();

    // EGL handles, stored as void* to keep <EGL/egl.h> out of the header.
    void* display_;
    void* shared_ctx_;

    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::unordered_set<std::uint64_t> in_flight_;
    std::atomic<bool> shutdown_{false};
    std::atomic<std::uint64_t> enqueued_count_{0};
    std::atomic<std::uint64_t> completed_count_{0};
};

// One-shot smoke test: builds an AsyncShaderCompiler, enqueues a handful of
// trivial programs, waits for completion (drains for the test), tears down.
// Idempotent; logs to stage=opengl.async.compiler.smoke.*.
void RunAsyncCompilerSmokeTest();

} // namespace OpenGL

#else

namespace OpenGL {
inline void RunAsyncCompilerSmokeTest() {}
} // namespace OpenGL

#endif
