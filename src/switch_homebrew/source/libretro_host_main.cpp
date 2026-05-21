// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "emulator_bootstrap.h"
#include "switch_input.h"
#include "switch_runtime.h"

#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
#include <EGL/egl.h>
#endif
#include <libretro.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace Azahar::Switch {
namespace {
std::atomic_bool g_libretro_game_framebuffer_ready{false};
}

void NotifyGameFramebufferSwap() {
    g_libretro_game_framebuffer_ready.store(true, std::memory_order_release);
}

bool HasGameFramebufferSwap() {
    return g_libretro_game_framebuffer_ready.load(std::memory_order_acquire);
}
} // namespace Azahar::Switch

namespace {

using Azahar::Switch::GameCatalog;

struct RuntimeState {
    bool mount_attempted = false;
    Result mount_result = 0;
    bool storage_ready = false;
    bool directories_ready = false;
    bool log_ready = false;
    int directory_errno = 0;
    int log_errno = 0;
    int frame_count = 0;
    int selected_game = 0;
    char status_message[192]{};
};

struct HostState {
    void* gl_context = nullptr;
    retro_hw_render_callback hw_render{};
    retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_XRGB8888;
    std::map<std::string, std::string> variables;
    bool variable_update = false;
    bool stop_requested = false;
    unsigned video_frames = 0;
    unsigned slow_run_logs = 0;
    unsigned slow_swap_logs = 0;
    unsigned swap_timing_samples = 0;
    unsigned input_change_logs = 0;
    std::uint16_t last_input_mask = 0;
};

HostState g_host;
constexpr const char* HostDirectory = "sdmc:/switch/azahar";
#if defined(ENABLE_DEKO3D) && !defined(AZAHAR_SWITCH_OPENGL_SPIKE)
constexpr const char* LibretroBuildMarker = "switch-libretro-deko-trace-off-v48";
#else
constexpr const char* LibretroBuildMarker = "switch-libretro-gl-hw-restore-v48";
#endif

#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
constexpr retro_hw_context_type SwitchHwContextType() {
#ifdef USING_GLES
    return RETRO_HW_CONTEXT_OPENGLES3;
#else
    return RETRO_HW_CONTEXT_OPENGL_CORE;
#endif
}

constexpr const char* SwitchHwContextName() {
#ifdef USING_GLES
    return "opengles3";
#else
    return "opengl-core";
#endif
}
#endif

const char* BoolText(bool value) {
    return value ? "ok" : "failed";
}

void LogMessage(enum retro_log_level, const char* format, ...) {
    char buffer[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Azahar::Switch::AppendLogFormat(nullptr, "libretro-log %s", buffer);
}

#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
retro_proc_address_t HostGetProcAddress(const char* symbol) {
    return reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(symbol));
}

uintptr_t HostGetCurrentFramebuffer() {
    return 0;
}
#endif

void CaptureCoreOptionDefaults(const retro_core_options_v2* options) {
    if (options == nullptr || options->definitions == nullptr) {
        return;
    }
    for (const auto* option = options->definitions; option->key != nullptr; ++option) {
        if (option->default_value != nullptr) {
            g_host.variables[option->key] = option->default_value;
        } else if (option->values[0].value != nullptr) {
            g_host.variables[option->key] = option->values[0].value;
        }
    }
    g_host.variables["citra_layout_option"] = "large_screen";
    g_host.variables["citra_large_screen_proportion"] = "2.25";
    g_host.variables["citra_resolution_factor"] = "1";
#if defined(ENABLE_DEKO3D) && !defined(AZAHAR_SWITCH_OPENGL_SPIKE)
    g_host.variables["citra_graphics_api"] = "Deko3D";
#else
    g_host.variables["citra_graphics_api"] = "OpenGL";
#endif
}

void CaptureCoreOptionDefaults(const retro_core_option_definition* options) {
    if (options == nullptr) {
        return;
    }
    for (const auto* option = options; option->key != nullptr; ++option) {
        if (option->default_value != nullptr) {
            g_host.variables[option->key] = option->default_value;
        } else if (option->values[0].value != nullptr) {
            g_host.variables[option->key] = option->values[0].value;
        }
    }
    g_host.variables["citra_layout_option"] = "large_screen";
    g_host.variables["citra_large_screen_proportion"] = "2.25";
    g_host.variables["citra_resolution_factor"] = "1";
#if defined(ENABLE_DEKO3D) && !defined(AZAHAR_SWITCH_OPENGL_SPIKE)
    g_host.variables["citra_graphics_api"] = "Deko3D";
#else
    g_host.variables["citra_graphics_api"] = "OpenGL";
#endif
}

void CaptureVariables(const retro_variable* variables) {
    if (variables == nullptr) {
        return;
    }
    for (const auto* variable = variables; variable->key != nullptr; ++variable) {
        if (variable->value == nullptr) {
            continue;
        }
        const char* values = std::strchr(variable->value, ';');
        values = values != nullptr ? values + 1 : variable->value;
        while (*values == ' ') {
            ++values;
        }
        const char* end = std::strchr(values, '|');
        g_host.variables[variable->key] =
            end != nullptr ? std::string(values, static_cast<std::size_t>(end - values))
                           : std::string(values);
    }
}

#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
bool EnsureOpenGLContext() {
    if (g_host.gl_context != nullptr) {
        u32 result = 0;
        return Azahar::Switch::Platform::MakeOpenGLCurrent(g_host.gl_context, &result);
    }

    u32 result = 0;
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.opengl.create begin");
    g_host.gl_context = Azahar::Switch::Platform::CreateOpenGLContext(1280, 720, &result);
    if (g_host.gl_context == nullptr) {
        Azahar::Switch::AppendLogFormat(nullptr,
                                        "android-flow stage=libretro.opengl.create result=%08X",
                                        result);
        return false;
    }
    if (!Azahar::Switch::Platform::MakeOpenGLCurrent(g_host.gl_context, &result)) {
        Azahar::Switch::AppendLogFormat(nullptr,
                                        "android-flow stage=libretro.opengl.current result=%08X",
                                        result);
        return false;
    }
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.opengl.ready");
    return true;
}
#endif

bool HostEnvironment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned*>(data) = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        CaptureCoreOptionDefaults(static_cast<const retro_core_options_v2*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        CaptureCoreOptionDefaults(static_cast<const retro_core_option_definition*>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        CaptureVariables(static_cast<const retro_variable*>(data));
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* variable = static_cast<retro_variable*>(data);
        if (variable == nullptr || variable->key == nullptr) {
            return true;
        }
        auto iter = g_host.variables.find(variable->key);
        variable->value = iter != g_host.variables.end() ? iter->second.c_str() : nullptr;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = g_host.variable_update;
        g_host.variable_update = false;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char**>(data) = HostDirectory;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        auto* log = static_cast<retro_log_callback*>(data);
        log->log = LogMessage;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        g_host.pixel_format = *static_cast<retro_pixel_format*>(data);
        return g_host.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
        *static_cast<retro_hw_context_type*>(data) = SwitchHwContextType();
#else
        *static_cast<retro_hw_context_type*>(data) = RETRO_HW_CONTEXT_NONE;
#endif
        return true;
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        auto* callback = static_cast<retro_hw_render_callback*>(data);
        Azahar::Switch::AppendLogFormat(
            nullptr, "android-flow stage=libretro.hw-render.request context=%u reset=%u",
            callback != nullptr ? static_cast<unsigned>(callback->context_type) : 0xFFFFFFFFU,
            callback != nullptr && callback->context_reset != nullptr ? 1U : 0U);
#if defined(ENABLE_DEKO3D) && !defined(AZAHAR_SWITCH_OPENGL_SPIKE)
        if (callback != nullptr && callback->context_type == RETRO_HW_CONTEXT_NONE &&
            callback->context_reset != nullptr) {
            g_host.hw_render = *callback;
            Azahar::Switch::AppendLogFormat(
                nullptr, "android-flow stage=libretro.hw-render context=deko3d-deferred");
            return true;
        }
#endif
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
        if (callback == nullptr || callback->context_type != SwitchHwContextType()) {
            return false;
        }
        if (!EnsureOpenGLContext()) {
            return false;
        }
        callback->get_proc_address = HostGetProcAddress;
        callback->get_current_framebuffer = HostGetCurrentFramebuffer;
        g_host.hw_render = *callback;
        Azahar::Switch::AppendLogFormat(
            nullptr, "android-flow stage=libretro.hw-render context=%s version=%u.%u",
            SwitchHwContextName(), callback->version_major, callback->version_minor);
        return true;
#else
        return false;
#endif
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        auto* message = static_cast<retro_message*>(data);
        if (message != nullptr && message->msg != nullptr) {
            Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.message text=\"%s\"",
                                            message->msg);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SHUTDOWN:
        Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.environment.shutdown");
        g_host.stop_requested = true;
        return true;
    default:
        return false;
    }
}

void HostVideoRefresh(const void* data, unsigned width, unsigned height, std::size_t pitch) {
    (void)pitch;
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    if (data == RETRO_HW_FRAME_BUFFER_VALID && g_host.gl_context != nullptr) {
        u32 result = 0;
        const bool trace_swap = g_host.swap_timing_samples++ < 32;
        const auto swap_start = trace_swap ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
        if (!Azahar::Switch::Platform::SwapOpenGLBuffers(g_host.gl_context, &result)) {
            Azahar::Switch::AppendLogFormat(nullptr,
                                            "android-flow stage=libretro.swap result=%08X",
                                            result);
        }
        const auto swap_ms =
            trace_swap ? std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - swap_start)
                             .count()
                       : 0;
        if (trace_swap && swap_ms >= 5 && g_host.slow_swap_logs < 32) {
            ++g_host.slow_swap_logs;
            Azahar::Switch::AppendLogFormat(nullptr,
                                            "android-flow stage=libretro.swap.slow frame=%u "
                                            "elapsed-ms=%lld",
                                            g_host.video_frames + 1,
                                            static_cast<long long>(swap_ms));
        }
    }
#endif
    ++g_host.video_frames;
    if (g_host.video_frames == 1 || g_host.video_frames == 60 ||
        (g_host.video_frames % 600) == 0) {
        Azahar::Switch::AppendLogFormat(nullptr,
                                        "android-flow stage=libretro.video-frame count=%u width=%u "
                                        "height=%u hw=%u",
                                        g_host.video_frames, width, height,
                                        data == RETRO_HW_FRAME_BUFFER_VALID ? 1U : 0U);
    }
}

void HostAudioSample(int16_t, int16_t) {}

std::size_t HostAudioBatch(const int16_t*, std::size_t frames) {
    return frames;
}

std::uint16_t CurrentJoypadMask() {
    using Azahar::Switch::SwitchButton;
    std::uint16_t mask = 0;
    const auto add = [&mask](unsigned id, SwitchButton button) {
        if (Azahar::Switch::IsSwitchButtonPressed(button)) {
            mask |= static_cast<std::uint16_t>(1u << id);
        }
    };

    add(RETRO_DEVICE_ID_JOYPAD_B, SwitchButton::B);
    add(RETRO_DEVICE_ID_JOYPAD_Y, SwitchButton::Y);
    add(RETRO_DEVICE_ID_JOYPAD_SELECT, SwitchButton::Minus);
    add(RETRO_DEVICE_ID_JOYPAD_START, SwitchButton::Plus);
    add(RETRO_DEVICE_ID_JOYPAD_UP, SwitchButton::Up);
    add(RETRO_DEVICE_ID_JOYPAD_DOWN, SwitchButton::Down);
    add(RETRO_DEVICE_ID_JOYPAD_LEFT, SwitchButton::Left);
    add(RETRO_DEVICE_ID_JOYPAD_RIGHT, SwitchButton::Right);
    add(RETRO_DEVICE_ID_JOYPAD_A, SwitchButton::A);
    add(RETRO_DEVICE_ID_JOYPAD_X, SwitchButton::X);
    add(RETRO_DEVICE_ID_JOYPAD_L, SwitchButton::L);
    add(RETRO_DEVICE_ID_JOYPAD_R, SwitchButton::R);
    add(RETRO_DEVICE_ID_JOYPAD_L2, SwitchButton::ZL);
    add(RETRO_DEVICE_ID_JOYPAD_R2, SwitchButton::ZR);
    return mask;
}

void LogInputMaskChange(const char* source, std::uint16_t mask) {
    if (mask == g_host.last_input_mask) {
        return;
    }

    if (g_host.input_change_logs < 240) {
        ++g_host.input_change_logs;
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=libretro.input source=%s mask=%04X prev=%04X frame=%u a=%u b=%u "
            "x=%u y=%u start=%u select=%u",
            source, static_cast<unsigned>(mask), static_cast<unsigned>(g_host.last_input_mask),
            g_host.video_frames, (mask & (1u << RETRO_DEVICE_ID_JOYPAD_A)) != 0 ? 1U : 0U,
            (mask & (1u << RETRO_DEVICE_ID_JOYPAD_B)) != 0 ? 1U : 0U,
            (mask & (1u << RETRO_DEVICE_ID_JOYPAD_X)) != 0 ? 1U : 0U,
            (mask & (1u << RETRO_DEVICE_ID_JOYPAD_Y)) != 0 ? 1U : 0U,
            (mask & (1u << RETRO_DEVICE_ID_JOYPAD_START)) != 0 ? 1U : 0U,
            (mask & (1u << RETRO_DEVICE_ID_JOYPAD_SELECT)) != 0 ? 1U : 0U);
    }
    g_host.last_input_mask = mask;
}

void HostInputPoll() {
    Azahar::Switch::UpdateSwitchInput();
    LogInputMaskChange("poll", CurrentJoypadMask());
}

int16_t AxisToRetro(float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(value * 32767.0f);
}

bool JoypadPressed(unsigned id) {
    using Azahar::Switch::SwitchButton;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_B:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::B);
    case RETRO_DEVICE_ID_JOYPAD_Y:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::Y);
    case RETRO_DEVICE_ID_JOYPAD_SELECT:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::Minus);
    case RETRO_DEVICE_ID_JOYPAD_START:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::Plus);
    case RETRO_DEVICE_ID_JOYPAD_UP:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::Up);
    case RETRO_DEVICE_ID_JOYPAD_DOWN:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::Down);
    case RETRO_DEVICE_ID_JOYPAD_LEFT:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::Left);
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::Right);
    case RETRO_DEVICE_ID_JOYPAD_A:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::A);
    case RETRO_DEVICE_ID_JOYPAD_X:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::X);
    case RETRO_DEVICE_ID_JOYPAD_L:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::L);
    case RETRO_DEVICE_ID_JOYPAD_R:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::R);
    case RETRO_DEVICE_ID_JOYPAD_L2:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::ZL);
    case RETRO_DEVICE_ID_JOYPAD_R2:
        return Azahar::Switch::IsSwitchButtonPressed(SwitchButton::ZR);
    default:
        return false;
    }
}

int16_t HostInputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) {
        return 0;
    }
    if (device == RETRO_DEVICE_JOYPAD) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            unsigned mask = 0;
            for (unsigned button = 0; button < 16; ++button) {
                if (JoypadPressed(button)) {
                    mask |= 1u << button;
                }
            }
            LogInputMaskChange("state-mask", static_cast<std::uint16_t>(mask));
            return static_cast<int16_t>(mask);
        }
        const bool pressed = JoypadPressed(id);
        if (id == RETRO_DEVICE_ID_JOYPAD_A && pressed) {
            LogInputMaskChange("state-a", CurrentJoypadMask());
        }
        return pressed ? 1 : 0;
    }
    if (device == RETRO_DEVICE_ANALOG) {
        const auto [x, y] = Azahar::Switch::GetSwitchStick(index);
        return id == RETRO_DEVICE_ID_ANALOG_X ? AxisToRetro(x) : AxisToRetro(y);
    }
    return 0;
}

RuntimeState InitializeRuntime() {
    RuntimeState state{};
    if (fsdevGetDeviceFileSystem("sdmc") == nullptr) {
        state.mount_attempted = true;
        state.mount_result = fsdevMountSdmc();
    }
    state.storage_ready = fsdevGetDeviceFileSystem("sdmc") != nullptr;
    state.directories_ready = Azahar::Switch::EnsureAppDirectories(&state.directory_errno);
    state.log_ready = Azahar::Switch::AppendLogFormat(&state.log_errno, "libretro-launch");
    std::snprintf(state.status_message, sizeof(state.status_message), "Ready");
    return state;
}

void PrintGameList(const GameCatalog& catalog, int selected_game) {
    if (catalog.scan_errno != 0) {
        std::printf("ROM scan failed: errno %d\n", catalog.scan_errno);
        return;
    }
    if (catalog.count == 0) {
        std::printf("No bootable files found.\n  %s\n", Azahar::Switch::GetRomDirectory());
        return;
    }
    const int count = static_cast<int>(catalog.count);
    int first = std::clamp(selected_game - 3, 0, count > 7 ? count - 7 : 0);
    for (int index = first; index < count && index < first + 7; ++index) {
        const auto& game = catalog.entries[index];
        std::printf("%c %-28.28s %s\n", index == selected_game ? '>' : ' ', game.name,
                    Azahar::Switch::GetFileTypeName(game.type, game.compressed));
    }
}

void PrintStatus(const RuntimeState& state, const GameCatalog& catalog) {
    std::printf("\x1b[2J\x1b[H");
    std::printf("Azahar Switch Libretro Host\n");
    std::printf("---------------------------\n\n");
    std::printf("Storage: %s  App dirs: %s  Log: %s\n", BoolText(state.storage_ready),
                BoolText(state.directories_ready), BoolText(state.log_ready));
    std::printf("ROMs: %zu  Frame: %d\n", catalog.count, state.frame_count);
    std::printf("Status: %s\n\n", state.status_message);
    PrintGameList(catalog, state.selected_game);
    std::printf("\nControls: Up/Down select  A start  X rescan  + exit\n");
}

void RunGame(const Azahar::Switch::GameCandidate& game) {
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.rungame.enter");
    Azahar::Switch::AppendLogFormat(nullptr, "azahar-libretro-start path=\"%s\" type=\"%s\"",
                                    game.path,
                                    Azahar::Switch::GetFileTypeName(game.type, game.compressed));
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.suspend-console.begin");
    Azahar::Switch::Platform::SuspendConsole();
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.suspend-console.end");

    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.host-reset.begin");
    g_host = HostState{};
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.host-reset.end");

    const std::uint32_t hos_version = Azahar::Switch::Platform::GetHosVersion();
    const auto cpu_boost_result = Azahar::Switch::Platform::SetCpuBoost(true);
    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=switch-cpu-boost mode=fast-load method=%u hos=%u.%u.%u result=%08X",
        static_cast<unsigned>(cpu_boost_result.method), (hos_version >> 16) & 0xFF,
        (hos_version >> 8) & 0xFF, hos_version & 0xFF, cpu_boost_result.result);

    const auto thread_priority_result = Azahar::Switch::Platform::SetThreadPriorityBoost(true);
    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=switch-thread-priority get=%08X set=%08X original=%u requested=%u "
        "active=%u",
        thread_priority_result.get_result, thread_priority_result.set_result,
        thread_priority_result.original_priority, thread_priority_result.requested_priority,
        thread_priority_result.active ? 1U : 0U);

    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.environment.begin");
    retro_set_environment(HostEnvironment);
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.environment.end");

    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callbacks.begin");
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callback.video.before");
    retro_set_video_refresh(HostVideoRefresh);
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callback.video.after");
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callback.audio-sample.before");
    retro_set_audio_sample(HostAudioSample);
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callback.audio-sample.after");
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callback.audio-batch.before");
    retro_set_audio_sample_batch(HostAudioBatch);
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callback.audio-batch.after");
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callback.input-poll.before");
    retro_set_input_poll(HostInputPoll);
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callback.input-poll.after");
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callback.input-state.before");
    retro_set_input_state(HostInputState);
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callback.input-state.after");
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.callbacks.end");

    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.init.begin");
    retro_init();
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.init.end");

    retro_game_info info{};
    info.path = game.path;
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.load.begin");
    auto stage_start = std::chrono::steady_clock::now();
    bool loaded = retro_load_game(&info);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - stage_start)
                          .count();
    Azahar::Switch::AppendLogFormat(nullptr,
                                    "android-flow stage=libretro.load result=%s elapsed-ms=%lld",
                                    loaded ? "ok" : "failed",
                                    static_cast<long long>(elapsed_ms));
#if defined(ENABLE_DEKO3D) && !defined(AZAHAR_SWITCH_OPENGL_SPIKE)
    if (loaded && g_host.hw_render.context_reset == nullptr &&
        g_host.variables["citra_graphics_api"] == "Deko3D") {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=libretro.context-reset.missing graphics=deko3d action=fail-fast");
        loaded = false;
    }
#endif
    if (loaded && g_host.hw_render.context_reset != nullptr) {
        Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.context-reset begin");
        stage_start = std::chrono::steady_clock::now();
        g_host.hw_render.context_reset();
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - stage_start)
                         .count();
        Azahar::Switch::AppendLogFormat(
            nullptr, "android-flow stage=libretro.context-reset end elapsed-ms=%lld",
            static_cast<long long>(elapsed_ms));
    }

    unsigned iterations = 0;
    const char* exit_reason = loaded ? "unknown" : "load-failed";
    auto timing_window_start = std::chrono::steady_clock::now();
    while (loaded && !g_host.stop_requested) {
        if (!appletMainLoop()) {
            exit_reason = "applet";
            break;
        }
        const bool trace_run = iterations < 32;
        const auto run_start = trace_run ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
        const unsigned frames_before = trace_run ? g_host.video_frames : 0;
        retro_run();
        const auto run_ms =
            trace_run ? std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - run_start)
                            .count()
                      : 0;
        ++iterations;
        if (trace_run && run_ms >= 1000 && g_host.slow_run_logs < 32) {
            ++g_host.slow_run_logs;
            Azahar::Switch::AppendLogFormat(nullptr,
                                            "android-flow stage=libretro.run.slow iteration=%u "
                                            "frames-before=%u frames-after=%u elapsed-ms=%lld",
                                            iterations, frames_before, g_host.video_frames,
                                            static_cast<long long>(run_ms));
        }
        if ((iterations % 600) == 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - timing_window_start)
                    .count();
            timing_window_start = now;
            Azahar::Switch::AppendLogFormat(nullptr,
                                            "android-flow stage=libretro.run progress=%u frames=%u "
                                            "window-ms=%lld",
                                            iterations, g_host.video_frames,
                                            static_cast<long long>(elapsed_ms));
        }
        if (Azahar::Switch::IsSwitchButtonPressed(Azahar::Switch::SwitchButton::Plus) &&
            Azahar::Switch::IsSwitchButtonPressed(Azahar::Switch::SwitchButton::Minus)) {
            exit_reason = "plus-minus";
            break;
        }
    }
    if (g_host.stop_requested) {
        exit_reason = "core-shutdown";
    }

    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=libretro.loop.exit reason=%s loaded=%u stop=%u iterations=%u frames=%u "
        "input=%04X",
        exit_reason, loaded ? 1U : 0U, g_host.stop_requested ? 1U : 0U, iterations,
        g_host.video_frames, static_cast<unsigned>(g_host.last_input_mask));

    if (loaded) {
        Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.unload.begin");
        retro_unload_game();
        Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.unload.end");
    }
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.deinit.begin");
    retro_deinit();
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.deinit.end");
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    if (g_host.gl_context != nullptr) {
        Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.gl-destroy.begin");
        Azahar::Switch::Platform::DestroyOpenGLContext(g_host.gl_context);
        g_host.gl_context = nullptr;
        Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.gl-destroy.end");
    }
#endif
    const auto thread_restore_result = Azahar::Switch::Platform::SetThreadPriorityBoost(false);
    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=switch-thread-priority.restore get=%08X set=%08X original=%u "
        "requested=%u active=%u",
        thread_restore_result.get_result, thread_restore_result.set_result,
        thread_restore_result.original_priority, thread_restore_result.requested_priority,
        thread_restore_result.active ? 1U : 0U);
    const auto cpu_restore_result = Azahar::Switch::Platform::SetCpuBoost(false);
    Azahar::Switch::AppendLogFormat(nullptr,
                                    "android-flow stage=switch-cpu-boost.restore method=%u "
                                    "result=%08X",
                                    static_cast<unsigned>(cpu_restore_result.method),
                                    cpu_restore_result.result);
    Azahar::Switch::AppendLogFormat(nullptr, "azahar-libretro-stop iterations=%u frames=%u",
                                    iterations, g_host.video_frames);
    consoleInit(nullptr);
}

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    consoleInit(nullptr);
    Azahar::Switch::RegisterSwitchInput();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    RuntimeState state = InitializeRuntime();
    Azahar::Switch::AppendLogFormat(nullptr, "libretro-build-marker id=%s",
                                    LibretroBuildMarker);
    GameCatalog catalog = Azahar::Switch::ScanGameDirectory();
    Azahar::Switch::AppendLogFormat(nullptr, "libretro-runtime-ready games=%zu", catalog.count);

    if (catalog.count == 1) {
        Azahar::Switch::AppendLogFormat(nullptr, "libretro-autostart path=\"%s\"",
                                        catalog.entries[0].path);
        Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.autostart.before-run");
        RunGame(catalog.entries[0]);
        Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=libretro.autostart.after-run");
        std::snprintf(state.status_message, sizeof(state.status_message), "Returned from core");
    }

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down_buttons = padGetButtonsDown(&pad);
        if (down_buttons != 0) {
            Azahar::Switch::AppendLogFormat(nullptr, "libretro-menu-input down=%016llX",
                                            static_cast<unsigned long long>(down_buttons));
        }

        if ((down_buttons & HidNpadButton_Plus) != 0) {
            Azahar::Switch::AppendLogFormat(nullptr, "libretro-exit-requested");
            break;
        }
        if ((down_buttons & HidNpadButton_AnyUp) != 0) {
            Azahar::Switch::MoveSelection(catalog, state.selected_game, -1);
        }
        if ((down_buttons & HidNpadButton_AnyDown) != 0) {
            Azahar::Switch::MoveSelection(catalog, state.selected_game, 1);
        }
        if ((down_buttons & HidNpadButton_X) != 0) {
            catalog = Azahar::Switch::ScanGameDirectory();
            state.selected_game = 0;
            std::snprintf(state.status_message, sizeof(state.status_message), "Rescanned ROMs");
        }
        if ((down_buttons & HidNpadButton_A) != 0 && catalog.count > 0 &&
            state.selected_game < static_cast<int>(catalog.count)) {
            RunGame(catalog.entries[state.selected_game]);
            std::snprintf(state.status_message, sizeof(state.status_message), "Returned from core");
        }

        ++state.frame_count;
        PrintStatus(state, catalog);
        consoleUpdate(nullptr);
    }

    Azahar::Switch::AppendLogFormat(nullptr, "libretro-shutdown");
    if (state.mount_attempted && R_SUCCEEDED(state.mount_result)) {
        fsdevUnmountAll();
    }
    consoleExit(nullptr);
    return 0;
}
