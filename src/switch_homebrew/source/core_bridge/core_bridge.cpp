// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "core_bridge/core_bridge.h"

#include "common/file_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/applets/default_applets.h"
#include "core/loader/loader.h"
#include "core_bridge/emu_window_switch.h"
#include "switch_input.h"
#include "switch_jit_probe.h"
#include "switch_runtime.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

namespace Azahar::Switch {
namespace {

std::atomic_bool g_game_framebuffer_seen{false};
std::atomic_bool g_game_framebuffer_ready{false};

constexpr u32 BootCpuClockPercent = 100;
constexpr u32 RuntimeCpuClockPercent = 100;
constexpr bool KeepCpuBoostDuringRun = true;

struct CpuBoostGuard {
    bool active{};

    ~CpuBoostGuard() {
        if (active) {
            Platform::SetCpuBoost(false);
        }
    }

    Platform::CpuBoostResult Restore() {
        Platform::CpuBoostResult result{};
        if (active) {
            result = Platform::SetCpuBoost(false);
            active = false;
        }
        return result;
    }
};

struct ThreadPriorityBoostGuard {
    bool active{};

    ~ThreadPriorityBoostGuard() {
        if (active) {
            Platform::SetThreadPriorityBoost(false);
        }
    }
};

const char* GetLoaderResultName(Loader::ResultStatus status) {
    switch (status) {
    case Loader::ResultStatus::Success:
        return "success";
    case Loader::ResultStatus::Error:
        return "error";
    case Loader::ResultStatus::ErrorInvalidFormat:
        return "error-invalid-format";
    case Loader::ResultStatus::ErrorNotImplemented:
        return "error-not-implemented";
    case Loader::ResultStatus::ErrorNotLoaded:
        return "error-not-loaded";
    case Loader::ResultStatus::ErrorNotUsed:
        return "error-not-used";
    case Loader::ResultStatus::ErrorAlreadyLoaded:
        return "error-already-loaded";
    case Loader::ResultStatus::ErrorMemoryAllocationFailed:
        return "error-memory-allocation-failed";
    case Loader::ResultStatus::ErrorEncrypted:
        return "error-encrypted";
    case Loader::ResultStatus::ErrorGbaTitle:
        return "error-gba-title";
    case Loader::ResultStatus::ErrorArtic:
        return "error-artic";
    case Loader::ResultStatus::ErrorNotFound:
        return "error-not-found";
    case Loader::ResultStatus::ErrorPatches:
        return "error-patches";
    case Loader::ResultStatus::ErrorPatchesInvalidTitle:
        return "error-patches-invalid-title";
    }
    return "unknown";
}

const char* GetSystemResultName(Core::System::ResultStatus status) {
    switch (status) {
    case Core::System::ResultStatus::Success:
        return "success";
    case Core::System::ResultStatus::ErrorNotInitialized:
        return "error-not-initialized";
    case Core::System::ResultStatus::ErrorGetLoader:
        return "error-get-loader";
    case Core::System::ResultStatus::ErrorSystemMode:
        return "error-system-mode";
    case Core::System::ResultStatus::ErrorLoader:
        return "error-loader";
    case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted:
        return "error-loader-encrypted";
    case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
        return "error-loader-invalid-format";
    case Core::System::ResultStatus::ErrorLoader_ErrorGbaTitle:
        return "error-loader-gba-title";
    case Core::System::ResultStatus::ErrorLoader_ErrorPatches:
        return "error-loader-patches";
    case Core::System::ResultStatus::ErrorLoader_ErrorPatchesInvalidTitle:
        return "error-loader-patches-invalid-title";
    case Core::System::ResultStatus::ErrorSystemFiles:
        return "error-system-files";
    case Core::System::ResultStatus::ErrorSavestate:
        return "error-savestate";
    case Core::System::ResultStatus::ErrorArticDisconnected:
        return "error-artic-disconnected";
    case Core::System::ResultStatus::ErrorN3DSApplication:
        return "error-n3ds-application";
    case Core::System::ResultStatus::ErrorCoreExceptionRaised:
        return "error-core-exception";
    case Core::System::ResultStatus::ErrorMemoryExceptionRaised:
        return "error-memory-exception";
    case Core::System::ResultStatus::ShutdownRequested:
        return "shutdown-requested";
    case Core::System::ResultStatus::ErrorUnknown:
        return "error-unknown";
    }
    return "unknown";
}

bool IsEncryptedLoadFailure(Core::System::ResultStatus status) {
    return status == Core::System::ResultStatus::ErrorLoader_ErrorEncrypted;
}

void SetMessage(CoreBridgeStatus& status, const char* format, const char* value) {
    std::snprintf(status.message, sizeof(status.message), format, value != nullptr ? value : "");
}

void ConfigureSwitchFrontendSettings() {
    Settings::values.graphics_api.SetGlobal(true);
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    Settings::values.graphics_api.SetValue(Settings::GraphicsAPI::OpenGL);
#else
    Settings::values.graphics_api.SetValue(Settings::GraphicsAPI::Deko3D);
#endif
    Settings::values.use_hw_shader.SetGlobal(true);
    Settings::values.use_hw_shader.SetValue(true);
    Settings::values.use_cpu_jit.SetValue(true);
    Settings::values.use_shader_jit.SetValue(true);
    Settings::values.cpu_clock_percentage.SetGlobal(true);
    Settings::values.cpu_clock_percentage.SetValue(BootCpuClockPercent);
    Settings::values.async_shader_compilation.SetGlobal(true);
    Settings::values.async_shader_compilation.SetValue(true);
    Settings::values.use_vsync.SetGlobal(true);
    Settings::values.use_vsync.SetValue(false);
    Settings::values.frame_limit.SetGlobal(true);
    Settings::values.frame_limit.SetValue(0.0);
    Settings::values.delay_game_render_thread_us.SetGlobal(true);
    Settings::values.delay_game_render_thread_us.SetValue(0);
    Settings::values.simulate_3ds_gpu_timings.SetGlobal(true);
    Settings::values.simulate_3ds_gpu_timings.SetValue(false);
    Settings::values.use_disk_shader_cache.SetGlobal(true);
    Settings::values.use_disk_shader_cache.SetValue(true);
    Settings::values.shaders_accurate_mul.SetGlobal(true);
    Settings::values.shaders_accurate_mul.SetValue(false);

    auto& input_profile = Settings::values.current_input_profile;
    input_profile.buttons[Settings::NativeButton::A] = GetSwitchButtonParam(SwitchButton::A);
    input_profile.buttons[Settings::NativeButton::B] = GetSwitchButtonParam(SwitchButton::B);
    input_profile.buttons[Settings::NativeButton::X] = GetSwitchButtonParam(SwitchButton::X);
    input_profile.buttons[Settings::NativeButton::Y] = GetSwitchButtonParam(SwitchButton::Y);
    input_profile.buttons[Settings::NativeButton::Up] = GetSwitchButtonParam(SwitchButton::Up);
    input_profile.buttons[Settings::NativeButton::Down] = GetSwitchButtonParam(SwitchButton::Down);
    input_profile.buttons[Settings::NativeButton::Left] = GetSwitchButtonParam(SwitchButton::Left);
    input_profile.buttons[Settings::NativeButton::Right] = GetSwitchButtonParam(SwitchButton::Right);
    input_profile.buttons[Settings::NativeButton::L] = GetSwitchButtonParam(SwitchButton::L);
    input_profile.buttons[Settings::NativeButton::R] = GetSwitchButtonParam(SwitchButton::R);
    input_profile.buttons[Settings::NativeButton::Start] = GetSwitchButtonParam(SwitchButton::Plus);
    input_profile.buttons[Settings::NativeButton::Select] = GetSwitchButtonParam(SwitchButton::Minus);
    input_profile.buttons[Settings::NativeButton::ZL] = GetSwitchButtonParam(SwitchButton::ZL);
    input_profile.buttons[Settings::NativeButton::ZR] = GetSwitchButtonParam(SwitchButton::ZR);
    input_profile.analogs[Settings::NativeAnalog::CirclePad] = GetSwitchAnalogParam(0);
    input_profile.analogs[Settings::NativeAnalog::CStick] = GetSwitchAnalogParam(1);
    input_profile.touch_device = "engine:emu_window";
    input_profile.controller_touch_device.clear();
    input_profile.use_touchpad = false;
    input_profile.use_touch_from_button = false;

    Settings::values.layout_option.SetGlobal(true);
    Settings::values.layout_option.SetValue(Settings::LayoutOption::LargeScreen);
    Settings::values.large_screen_proportion.SetGlobal(true);
    Settings::values.large_screen_proportion.SetValue(2.25f);
    Settings::values.small_screen_position.SetGlobal(true);
    Settings::values.small_screen_position.SetValue(Settings::SmallScreenPosition::TopRight);
    Settings::values.screen_gap.SetGlobal(true);
    Settings::values.screen_gap.SetValue(0);
    Settings::values.swap_screen.SetGlobal(true);
    Settings::values.swap_screen.SetValue(false);
    Settings::values.upright_screen.SetGlobal(true);
    Settings::values.upright_screen.SetValue(false);
    Settings::values.use_integer_scaling.SetGlobal(true);
    Settings::values.use_integer_scaling.SetValue(false);
}

} // namespace

void NotifyGameFramebufferSwap() {
    g_game_framebuffer_seen.store(true, std::memory_order_release);
    g_game_framebuffer_ready.store(true, std::memory_order_release);
}

bool HasGameFramebufferSwap() {
    return g_game_framebuffer_ready.load(std::memory_order_acquire);
}

const char* GetCoreBridgeResultName(CoreBridgeResult result) {
    switch (result) {
    case CoreBridgeResult::Success:
        return "success";
    case CoreBridgeResult::MissingPath:
        return "missing-path";
    case CoreBridgeResult::LoaderUnavailable:
        return "loader-unavailable";
    case CoreBridgeResult::LoadFailed:
        return "load-failed";
    case CoreBridgeResult::RunLoopFailed:
        return "runloop-failed";
    case CoreBridgeResult::ShutdownRequested:
        return "shutdown-requested";
    }
    return "unknown";
}

CoreBridgeStatus StartCoreBridge(const GameCandidate& game) {
    CoreBridgeStatus status{};

    if (game.path[0] == '\0') {
        status.result = CoreBridgeResult::MissingPath;
        SetMessage(status, "%s", "No bootable file selected");
        return status;
    }

    Core::System& system = Core::System::GetInstance();
    FileUtil::SetCurrentRomPath(game.path);
    auto loader = Loader::GetLoader(game.path);
    if (!loader) {
        status.result = CoreBridgeResult::LoaderUnavailable;
        SetMessage(status, "Loader unavailable: %s", game.path);
        return status;
    }

    u64 program_id{};
    const Loader::ResultStatus program_id_result = loader->ReadProgramId(program_id);
    AppendLogFormat(nullptr, "android-flow stage=loader-program-id result=%s value=%016llX",
                    GetLoaderResultName(program_id_result),
                    static_cast<unsigned long long>(program_id));

    const auto memory_mode = loader->LoadKernelMemoryMode();
    AppendLogFormat(nullptr,
                    "android-flow stage=loader-memory-mode result=%s has-value=%d value=%u",
                    GetLoaderResultName(memory_mode.second), memory_mode.first.has_value() ? 1 : 0,
                    memory_mode.first.has_value() ? static_cast<unsigned>(*memory_mode.first) : 0);

    ConfigureSwitchFrontendSettings();
    AppendLogFormat(nullptr, "android-flow stage=switch-android-parity.default-applets begin");
    Frontend::RegisterDefaultApplets(system);
    AppendLogFormat(nullptr, "android-flow stage=switch-android-parity.default-applets end");
    AppendLogFormat(nullptr, "android-flow stage=switch-android-parity.register-loader begin");
    system.RegisterAppLoaderEarly(loader);
    AppendLogFormat(nullptr, "android-flow stage=switch-android-parity.register-loader end");
    AppendLogFormat(nullptr, "android-flow stage=switch-android-parity.apply-settings begin");
    system.ApplySettings();
    AppendLogFormat(nullptr, "android-flow stage=switch-android-parity.apply-settings end");
    AppendLogFormat(nullptr, "android-flow stage=switch-renderer result=%s",
                    Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::OpenGL
                        ? "opengl"
                        : "deko3d");
    AppendLogFormat(nullptr, "android-flow stage=switch-cpu-jit enabled=%d",
                    Settings::values.use_cpu_jit.GetValue() ? 1 : 0);
    AppendLogFormat(nullptr,
                    "android-flow stage=switch-speed frame-limit=0 vsync=0 gpu-timings=0 "
                    "cpu-clock=%u runtime-cpu-clock=%u "
                    "hw-shaders=1 async-shaders=1 disk-shader-cache=1 accurate-mul=0 "
                    "pica-shader-jit=1 new3ds-mode=1 dynarmic-page-table=1 "
                    "dynarmic-code-cache-mb=64 dynarmic-fast-ticks=0 switch-slice-cap=50000 "
                    "apm-fast-load=keep-during-run android-startup-parity=1",
                    BootCpuClockPercent, RuntimeCpuClockPercent);
    const std::uint32_t hos_version = Platform::GetHosVersion();
    const bool cpu_boost_eligible = hos_version >= (7u << 16);
    const Platform::CpuBoostResult cpu_boost_result = Platform::SetCpuBoost(true);
    CpuBoostGuard cpu_boost_guard{cpu_boost_result.result == 0};
    AppendLogFormat(nullptr,
                    "android-flow stage=switch-cpu-boost mode=fast-load method=%u hos=%u.%u.%u "
                    "eligible=%d result=%08X",
                    static_cast<unsigned>(cpu_boost_result.method),
                    (hos_version >> 16) & 0xFF, (hos_version >> 8) & 0xFF, hos_version & 0xFF,
                    cpu_boost_eligible ? 1 : 0, cpu_boost_result.result);
    const Platform::ThreadPriorityBoostResult thread_priority_result =
        Platform::SetThreadPriorityBoost(true);
    ThreadPriorityBoostGuard thread_priority_boost_guard{thread_priority_result.active};
    AppendLogFormat(nullptr,
                    "android-flow stage=switch-thread-priority get=%08X set=%08X original=%u "
                    "requested=%u active=%d",
                    thread_priority_result.get_result, thread_priority_result.set_result,
                    thread_priority_result.original_priority, thread_priority_result.requested_priority,
                    thread_priority_result.active ? 1 : 0);
    AppendLogFormat(nullptr, "android-flow stage=switch-layout result=android-landscape");

    EmuWindow_Switch window;
    bool console_suspended = false;
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    if (Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::OpenGL) {
        AppendLogFormat(nullptr, "android-flow stage=switch-console result=suspended-for-opengl");
        Platform::SuspendConsole();
        console_suspended = true;
    }
#endif
    AppendLogFormat(nullptr, "android-flow stage=switch-opengl.preflight begin");
    window.MakeCurrent();
    AppendLogFormat(nullptr, "android-flow stage=switch-opengl.preflight end");
    if (Settings::values.use_cpu_jit.GetValue()) {
        const bool jit_probe_ok = RunSwitchJitProbe();
        AppendLogFormat(nullptr, "android-flow stage=switch-jit-probe result=%s",
                        jit_probe_ok ? "ok" : "failed");
    }

    const Core::System::ResultStatus load_result = system.Load(window, game.path);
    AppendLogFormat(nullptr, "android-flow stage=system-load result=%s",
                    GetSystemResultName(load_result));

    if (load_result != Core::System::ResultStatus::Success) {
        status.result = CoreBridgeResult::LoadFailed;
        if (IsEncryptedLoadFailure(load_result)) {
            SetMessage(status, "%s", "ROM appears encrypted");
        } else {
            SetMessage(status, "Core load failed: %s", GetSystemResultName(load_result));
        }
        system.Shutdown();
        return status;
    }

    AppendLogFormat(nullptr, "android-flow stage=switch-android-parity.per-game-gpu begin");
    system.GPU().ApplyPerProgramSettings(program_id);
    AppendLogFormat(nullptr, "android-flow stage=switch-android-parity.per-game-gpu end");
    AppendLogFormat(nullptr, "android-flow stage=switch-android-parity.disk-resources begin");
    system.GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(false, nullptr);
    AppendLogFormat(nullptr, "android-flow stage=switch-android-parity.disk-resources end");

    if (!console_suspended) {
        AppendLogFormat(nullptr, "android-flow stage=switch-console result=suspended");
        Platform::SuspendConsole();
        console_suspended = true;
    }
    window.PresentLoadingFrame(0);

    Core::System::ResultStatus run_result = Core::System::ResultStatus::Success;
    u32 iterations = 0;
    u32 slow_runloop_log_count = 0;
    bool logged_runtime_cpu_boost = false;
    g_game_framebuffer_seen.store(false, std::memory_order_release);
    g_game_framebuffer_ready.store(false, std::memory_order_release);
    Platform::InitializeFramePumpInput();
    AppendLogFormat(nullptr, "android-flow stage=runloop.begin mode=frame-pump");
    while (Platform::AppletMainLoop()) {
        if (Platform::FramePumpStopRequested()) {
            AppendLogFormat(nullptr, "android-flow stage=runloop.stop-requested iterations=%u",
                            iterations);
            break;
        }

        if (iterations == 0) {
            AppendLogFormat(nullptr, "android-flow stage=runloop.first-step.begin");
        }
        const auto runloop_start = std::chrono::steady_clock::now();
        run_result = system.RunLoop(true);
        const auto runloop_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - runloop_start)
                                            .count();
        if (runloop_elapsed_ms >= 1000 &&
            (slow_runloop_log_count < 16 || runloop_elapsed_ms >= 5000)) {
            ++slow_runloop_log_count;
            AppendLogFormat(nullptr,
                            "android-flow stage=runloop.slow iterations=%u elapsed-ms=%lld",
                            iterations, static_cast<long long>(runloop_elapsed_ms));
        }
        if (iterations == 0) {
            AppendLogFormat(nullptr, "android-flow stage=runloop.first-step.end result=%s",
                            GetSystemResultName(run_result));
        }
        ++iterations;
        if (!logged_runtime_cpu_boost && cpu_boost_guard.active &&
            g_game_framebuffer_seen.exchange(false, std::memory_order_acq_rel)) {
            logged_runtime_cpu_boost = true;
            if constexpr (KeepCpuBoostDuringRun) {
                AppendLogFormat(nullptr,
                                "android-flow stage=switch-cpu-boost mode=keep-during-run "
                                "trigger=buffer-swap cpu-clock=%u iterations=%u",
                                RuntimeCpuClockPercent, iterations);
            } else {
                const Platform::CpuBoostResult restore_result = cpu_boost_guard.Restore();
                Settings::values.cpu_clock_percentage.SetValue(RuntimeCpuClockPercent);
                system.CoreTiming().UpdateClockSpeed(RuntimeCpuClockPercent);
                AppendLogFormat(nullptr,
                                "android-flow stage=switch-cpu-boost mode=restore-after-first-frame "
                                "method=%u result=%08X trigger=buffer-swap cpu-clock=%u iterations=%u",
                                static_cast<unsigned>(restore_result.method), restore_result.result,
                                RuntimeCpuClockPercent, iterations);
            }
        }
        if (!window.HasPresentedSoftwareFrame() && (iterations % 4096) == 0) {
            window.PresentLoadingFrame(iterations);
        }

        if (run_result != Core::System::ResultStatus::Success) {
            break;
        }
        if (iterations == 4096 || (iterations % 262144) == 0) {
            AppendLogFormat(nullptr, "android-flow stage=runloop.progress iterations=%u",
                            iterations);
        }
    }

    AppendLogFormat(nullptr, "android-flow stage=runloop result=%s iterations=%u",
                    GetSystemResultName(run_result), iterations);

    if (run_result == Core::System::ResultStatus::ShutdownRequested) {
        status.result = CoreBridgeResult::ShutdownRequested;
        SetMessage(status, "%s", "Core requested shutdown");
    } else if (run_result != Core::System::ResultStatus::Success) {
        status.result = CoreBridgeResult::RunLoopFailed;
        SetMessage(status, "RunLoop failed: %s", GetSystemResultName(run_result));
    } else {
        status.result = CoreBridgeResult::Success;
        SetMessage(status, "%s", "Core frame pump stopped");
    }

    system.Shutdown();
    return status;
}

void StopCoreBridge() {
    Core::System::GetInstance().RequestShutdown();
}

} // namespace Azahar::Switch
