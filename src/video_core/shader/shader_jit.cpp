// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/arch.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)

#ifdef __SWITCH__
#include <atomic>
#include <chrono>
#endif

#include "common/assert.h"
#include "common/hash.h"
#include "common/microprofile.h"
#include "video_core/shader/shader.h"
#include "video_core/shader/shader_jit.h"
#if CITRA_ARCH(arm64)
#include "video_core/shader/shader_jit_a64_compiler.h"
#endif
#if CITRA_ARCH(x86_64)
#include "video_core/shader/shader_jit_x64_compiler.h"
#endif

#ifdef __SWITCH__
namespace Azahar::Switch {
bool AppendLogFormat(int* error_out, const char* format, ...);
}
#endif

namespace Pica::Shader {

#ifdef __SWITCH__
namespace {
std::atomic<unsigned> switch_shader_jit_trace_logs{};
constexpr unsigned SwitchShaderJitTraceLogLimit = 32;

bool SwitchShaderJitReserveLog() {
    return switch_shader_jit_trace_logs.fetch_add(1, std::memory_order_relaxed) <
           SwitchShaderJitTraceLogLimit;
}

long long SwitchShaderJitElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}
} // namespace
#endif

JitEngine::JitEngine() = default;
JitEngine::~JitEngine() = default;

void JitEngine::SetupBatch(ShaderSetup& setup, u32 entry_point) {
    ASSERT(entry_point < MAX_PROGRAM_CODE_LENGTH);
    setup.entry_point = entry_point;

    setup.DoProgramCodeFixup();
    const u64 code_hash = setup.GetProgramCodeHash();
    const u64 swizzle_hash = setup.GetSwizzleDataHash();

    const u64 cache_key = Common::HashCombine(code_hash, swizzle_hash);
    auto iter = cache.find(cache_key);
    if (iter != cache.end()) {
        setup.cached_shader = iter->second.get();
    } else {
        auto shader = std::make_unique<JitShader>();
#ifdef __SWITCH__
        const auto switch_compile_start = std::chrono::steady_clock::now();
#endif
        shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());
#ifdef __SWITCH__
        const auto switch_compile_ms = SwitchShaderJitElapsedMs(switch_compile_start);
        if (switch_compile_ms >= 5 && SwitchShaderJitReserveLog()) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=shaderjit.compile elapsed-ms=%lld code=%016llX "
                "swizzle=%016llX cache-before=%zu",
                switch_compile_ms, static_cast<unsigned long long>(code_hash),
                static_cast<unsigned long long>(swizzle_hash), cache.size());
        }
#endif
        setup.cached_shader = shader.get();
        cache.emplace_hint(iter, cache_key, std::move(shader));
    }
}

MICROPROFILE_DECLARE(GPU_Shader);

void JitEngine::Run(const ShaderSetup& setup, ShaderUnit& state) const {
    ASSERT(setup.cached_shader != nullptr);

    MICROPROFILE_SCOPE(GPU_Shader);

    const JitShader* shader = static_cast<const JitShader*>(setup.cached_shader);
#ifdef __SWITCH__
    const bool switch_trace = SwitchShaderJitReserveLog();
    const auto switch_run_start =
        switch_trace ? std::chrono::steady_clock::now()
                     : std::chrono::steady_clock::time_point{};
#endif
    shader->Run(setup, state, setup.entry_point);
#ifdef __SWITCH__
    const auto switch_run_ms = switch_trace ? SwitchShaderJitElapsedMs(switch_run_start) : 0;
    if (switch_trace && switch_run_ms >= 5) {
        Azahar::Switch::AppendLogFormat(
            nullptr, "android-flow stage=shaderjit.run.slow elapsed-ms=%lld entry=%u",
            switch_run_ms, setup.entry_point);
    }
#endif
}

} // namespace Pica::Shader

#endif // CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
