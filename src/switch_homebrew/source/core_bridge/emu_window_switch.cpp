// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "core_bridge/emu_window_switch.h"

#include "common/settings.h"
#include "core/core.h"
#include "emulator_bootstrap.h"
#include "switch_input.h"
#include "switch_runtime.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_software/renderer_software.h"

#include <algorithm>
#include <cstring>
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
#include <glad/glad.h>
#endif

namespace Azahar::Switch {

bool HasGameFramebufferSwap();

namespace {

bool HasRgbContent(const SwRenderer::ScreenInfo& info) {
    if (info.pixels.empty()) {
        return false;
    }

    const std::size_t pixel_count = info.pixels.size() / 4;
    const std::size_t step = std::max<std::size_t>(1, pixel_count / 1024);
    for (std::size_t pixel = 0; pixel < pixel_count; pixel += step) {
        const std::size_t offset = pixel * 4;
        if (info.pixels[offset] != 0 || info.pixels[offset + 1] != 0 ||
            info.pixels[offset + 2] != 0) {
            return true;
        }
    }
    return false;
}

void FillWaitingPattern(u8* frame, u32 stride, u32 tick) {
    constexpr u32 width = 1280;
    constexpr u32 height = 720;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 band = ((x / 160) + (y / 120) + (tick / 256)) % 6;
            const u8 r = band == 0 || band == 3 ? 0x28 : band == 1 ? 0xB8 : 0x10;
            const u8 g = band == 1 || band == 4 ? 0x90 : band == 2 ? 0xC8 : 0x20;
            const u8 b = band == 2 || band == 5 ? 0xD8 : band == 0 ? 0x80 : 0x30;
            u8* dst = frame + static_cast<std::size_t>(y) * stride +
                      static_cast<std::size_t>(x) * 4;
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst[3] = 0xFF;
        }
    }
}

#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
class SwitchOpenGLSharedContext final : public Frontend::GraphicsContext {
public:
    explicit SwitchOpenGLSharedContext(void* context_) : context(context_) {}

    ~SwitchOpenGLSharedContext() override {
        Platform::DestroyOpenGLContext(context);
    }

    bool IsGLES() override {
        return false;
    }

    void MakeCurrent() override {
        u32 result = 0;
        if (!Platform::MakeOpenGLCurrent(context, &result)) {
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.child.make-current result=%08X",
                            result);
        }
    }

    void DoneCurrent() override {
        Platform::ClearOpenGLCurrent(context);
    }

private:
    void* context = nullptr;
};
#endif

} // namespace

EmuWindow_Switch::EmuWindow_Switch(unsigned width, unsigned height) {
    window_info.type = Frontend::WindowSystemType::Headless;
    window_info.render_surface = nullptr;
    window_info.render_surface_scale = 1.0f;
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    strict_context_required = true;
#endif
    SetFramebufferSize(width, height);
}

EmuWindow_Switch::~EmuWindow_Switch() {
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    if (gl_core_context != nullptr) {
        Platform::DestroyOpenGLContext(gl_core_context);
    }
    if (gl_context != nullptr) {
        Platform::DestroyOpenGLContext(gl_context);
    }
#endif
    if (deko_ready) {
        Platform::DestroyDekoPresenter(deko_presenter);
    }
    if (framebuffer_ready) {
        Platform::DestroyFramebuffer(framebuffer);
    }
}

bool EmuWindow_Switch::InitializeFramebuffer() {
    if (framebuffer_ready) {
        return true;
    }
    if (framebuffer_failed) {
        return false;
    }

    u32 result = 0;
    framebuffer = Platform::CreateLinearFramebuffer(1280, 720, &result);
    if (framebuffer == nullptr) {
        framebuffer_failed = true;
        AppendLogFormat(nullptr, "android-flow stage=switch-framebuffer.create result=%08X",
                        result);
        return false;
    }

    framebuffer_ready = true;
    AppendLogFormat(nullptr, "android-flow stage=switch-framebuffer.ready");
    return true;
}

bool EmuWindow_Switch::EnsureOpenGLPresenterContext() {
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    if (Settings::values.graphics_api.GetValue() != Settings::GraphicsAPI::OpenGL || gl_failed) {
        return false;
    }
    if (gl_ready) {
        return true;
    }

    u32 result = 0;
    AppendLogFormat(nullptr, "android-flow stage=switch-opengl.presenter.create begin");
    gl_context = Platform::CreateOpenGLContext(1280, 720, &result);
    if (gl_context == nullptr) {
        gl_failed = true;
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.presenter.create result=%08X",
                        result);
        return false;
    }

    gl_ready = true;
    AppendLogFormat(nullptr, "android-flow stage=switch-opengl.presenter.ready api=gl-core-4.3");
    return true;
#else
    return false;
#endif
}

bool EmuWindow_Switch::MakeOpenGLPresenterCurrent() {
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    if (!EnsureOpenGLPresenterContext()) {
        return false;
    }

    u32 result = 0;
    if (!Platform::MakeOpenGLCurrent(gl_context, &result)) {
        gl_failed = true;
        AppendLogFormat(nullptr,
                        "android-flow stage=switch-opengl.presenter.make-current result=%08X",
                        result);
        return false;
    }
    return true;
#else
    return false;
#endif
}

void EmuWindow_Switch::ProcessSwitchTouch() {
    u32 x = 0;
    u32 y = 0;
    if (!GetSwitchTouchPoint(&x, &y)) {
        if (switch_touch_active) {
            TouchReleased();
            switch_touch_active = false;
        }
        return;
    }

    if (switch_touch_active) {
        TouchMoved(x, y);
        return;
    }

    switch_touch_active = TouchPressed(x, y);
}

bool EmuWindow_Switch::IsGLES() {
    return false;
}

void EmuWindow_Switch::MakeCurrent() {
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    if (!EnsureOpenGLPresenterContext()) {
        return;
    }

    if (!gl_core_ready) {
        u32 result = 0;
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.core.create begin");
        gl_core_context = Platform::CreateOpenGLSharedContext(gl_context, &result);
        if (gl_core_context == nullptr) {
            gl_core_ready = true;
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.core.fallback presenter=1 result=%08X",
                            result);
        } else {
            gl_core_ready = true;
            AppendLogFormat(nullptr, "android-flow stage=switch-opengl.core.ready shared=1");
        }
    }

    u32 result = 0;
    void* core_context = gl_core_context != nullptr ? gl_core_context : gl_context;
    if (!Platform::MakeOpenGLCurrent(core_context, &result)) {
        if (gl_core_context != nullptr) {
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.core.make-current fallback=presenter result=%08X",
                            result);
            Platform::DestroyOpenGLContext(gl_core_context);
            gl_core_context = nullptr;
            if (Platform::MakeOpenGLCurrent(gl_context, &result)) {
                return;
            }
        }
        gl_failed = true;
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.core.make-current result=%08X",
                        result);
    }
#endif
}

void EmuWindow_Switch::DoneCurrent() {
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    if (gl_core_context != nullptr) {
        Platform::ClearOpenGLCurrent(gl_core_context);
    } else if (gl_context != nullptr) {
        Platform::ClearOpenGLCurrent(gl_context);
    }
#endif
}

void EmuWindow_Switch::SwapBuffers() {
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    if (!gl_ready || gl_failed) {
        return;
    }

    u32 result = 0;
    if (!Platform::SwapOpenGLBuffers(gl_context, &result)) {
        gl_failed = true;
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.swap result=%08X", result);
        return;
    }
    ++gl_present_count;
    if (gl_present_count <= 3 || gl_present_count == 60) {
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.swap-present count=%u",
                        gl_present_count);
    }
#endif
}

std::unique_ptr<Frontend::GraphicsContext> EmuWindow_Switch::CreateSharedContext() const {
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    auto* self = const_cast<EmuWindow_Switch*>(this);
    if (!self->EnsureOpenGLPresenterContext()) {
        return nullptr;
    }

    u32 result = 0;
    void* shared_context = Platform::CreateOpenGLSharedContext(self->gl_context, &result);
    if (shared_context == nullptr) {
        AppendLogFormat(nullptr,
                        "android-flow stage=switch-opengl.child.create result=%08X",
                        result);
        return nullptr;
    }

    AppendLogFormat(nullptr, "android-flow stage=switch-opengl.child.ready shared=1");
    return std::make_unique<SwitchOpenGLSharedContext>(shared_context);
#else
    return nullptr;
#endif
}

void EmuWindow_Switch::BlitScreen(u8* frame, u32 stride, const SwRenderer::ScreenInfo& info,
                                  const Common::Rectangle<u32>& rect) {
    if (info.pixels.empty() || rect.GetWidth() == 0 || rect.GetHeight() == 0) {
        return;
    }

    const u32 native_width = info.height;
    const u32 native_height = info.width;
    const u32 rect_width = rect.GetWidth();
    const u32 rect_height = rect.GetHeight();
    const u32 x_step = (native_width << 16) / rect_width;
    const u32 y_step = (native_height << 16) / rect_height;

    u32 src_y_fp = 0;
    for (u32 y = 0; y < rect_height; ++y) {
        const u32 src_y = src_y_fp >> 16;
        const std::size_t src_row_offset = static_cast<std::size_t>(src_y) * info.height * 4;
        if (src_row_offset >= info.pixels.size()) {
            src_y_fp += y_step;
            continue;
        }
        const u8* src_row = info.pixels.data() + src_row_offset;
        u32 src_x_fp = 0;
        for (u32 x = 0; x < rect_width; ++x) {
            const u32 src_x = src_x_fp >> 16;
            const std::size_t src_offset = src_row_offset + static_cast<std::size_t>(src_x) * 4;
            if (src_offset + 3 >= info.pixels.size()) {
                src_x_fp += x_step;
                continue;
            }

            u8* dst = frame + static_cast<std::size_t>(rect.top + y) * stride +
                      static_cast<std::size_t>(rect.left + x) * 4;
            const u8* src = src_row + static_cast<std::size_t>(src_x) * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 0xFF;
            src_x_fp += x_step;
        }
        src_y_fp += y_step;
    }
}

bool EmuWindow_Switch::PresentDekoFrame(const SwRenderer::ScreenInfo& top_screen,
                                        const SwRenderer::ScreenInfo& bottom_screen,
                                        const Layout::FramebufferLayout& layout) {
    if (deko_failed) {
        return false;
    }

    if (!deko_ready) {
        if (framebuffer_ready) {
            Platform::DestroyFramebuffer(framebuffer);
            framebuffer = nullptr;
            framebuffer_ready = false;
        }

        u32 result = 0;
        deko_presenter = Platform::CreateDekoPresenter(1280, 720, &result);
        if (deko_presenter == nullptr) {
            deko_failed = true;
            AppendLogFormat(nullptr, "android-flow stage=switch-deko.create result=%u", result);
            return false;
        }
        deko_ready = true;
        AppendLogFormat(nullptr, "android-flow stage=switch-deko.ready mode=async-present");
    }

    const auto make_screen = [](const SwRenderer::ScreenInfo& info,
                                const Common::Rectangle<u32>& rect,
                                bool enabled) -> Platform::DekoSoftwareScreen {
        return Platform::DekoSoftwareScreen{
            .pixels = info.pixels.empty() ? nullptr : info.pixels.data(),
            .width = info.height,
            .height = info.width,
            .dst =
                {
                    .left = rect.left,
                    .top = rect.top,
                    .right = rect.right,
                    .bottom = rect.bottom,
                },
            .enabled = enabled && !info.pixels.empty() && rect.GetWidth() > 0 &&
                       rect.GetHeight() > 0,
        };
    };

    u32 result = 0;
    const bool presented = Platform::PresentDekoSoftwareFrame(
        deko_presenter, make_screen(top_screen, layout.top_screen, layout.top_screen_enabled),
        make_screen(bottom_screen, layout.bottom_screen, layout.bottom_screen_enabled), &result);
    if (!presented) {
        deko_failed = true;
        AppendLogFormat(nullptr, "android-flow stage=switch-deko.present result=%u", result);
        return false;
    }

    ++deko_present_count;
    if (deko_present_count <= 3 || (deko_present_count % 60) == 0) {
        AppendLogFormat(nullptr, "android-flow stage=switch-deko.present count=%u",
                        deko_present_count);
    }
    return true;
}

void EmuWindow_Switch::PresentSoftwareFrame() {
    auto& renderer =
        static_cast<SwRenderer::RendererSoftware&>(Core::System::GetInstance().GPU().Renderer());
    const auto& layout = GetFramebufferLayout();
    const auto& top_screen = renderer.Screen(VideoCore::ScreenId::TopLeft);
    const auto& bottom_screen = renderer.Screen(VideoCore::ScreenId::Bottom);
    const bool top_has_rgb = HasRgbContent(top_screen);
    const bool bottom_has_rgb = HasRgbContent(bottom_screen);
    const bool frame_has_rgb = top_has_rgb || bottom_has_rgb;
    software_frame_has_rgb = software_frame_has_rgb || frame_has_rgb;
    ++present_count;
    if (present_count <= 3 || (present_count % 60) == 0) {
        AppendLogFormat(nullptr,
                        "android-flow stage=switch-frame.present count=%u top=%ux%u "
                        "top-bytes=%lu top-rgb=%u bottom=%ux%u bottom-bytes=%lu bottom-rgb=%u",
                        present_count, top_screen.width, top_screen.height,
                        static_cast<unsigned long>(top_screen.pixels.size()),
                        top_has_rgb ? 1 : 0, bottom_screen.width,
                        bottom_screen.height,
                        static_cast<unsigned long>(bottom_screen.pixels.size()),
                        bottom_has_rgb ? 1 : 0);
    }

    if (!frame_has_rgb) {
        if (present_count <= 3 || (present_count % 60) == 0) {
            AppendLogFormat(nullptr, "android-flow stage=switch-frame.blank count=%u",
                            present_count);
        }
        return;
    }

    if (PresentDekoFrame(top_screen, bottom_screen, layout)) {
        return;
    }

    if (!InitializeFramebuffer()) {
        return;
    }

    u32 stride = 0;
    u8* frame = Platform::BeginFramebuffer(framebuffer, &stride);
    if (frame == nullptr) {
        return;
    }

    std::memset(frame, 0, static_cast<std::size_t>(stride) * 720);

    if (layout.top_screen_enabled) {
        BlitScreen(frame, stride, top_screen, layout.top_screen);
    }
    if (layout.bottom_screen_enabled) {
        BlitScreen(frame, stride, bottom_screen, layout.bottom_screen);
    }

    Platform::EndFramebuffer(framebuffer);
}

void EmuWindow_Switch::PollEvents() {
    ProcessSwitchTouch();

    if (Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::Software) {
        PresentSoftwareFrame();
    }
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
    if (Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::OpenGL) {
        return;
    }
#endif
}

void EmuWindow_Switch::SetFramebufferSize(unsigned width, unsigned height) {
    UpdateCurrentFramebufferLayout(width, height, false);
    const auto& layout = GetFramebufferLayout();
    AppendLogFormat(nullptr,
                    "android-flow stage=switch-layout.rects width=%u height=%u top=%u,%u,%u,%u "
                    "bottom=%u,%u,%u,%u",
                    layout.width, layout.height, layout.top_screen.left, layout.top_screen.top,
                    layout.top_screen.right, layout.top_screen.bottom, layout.bottom_screen.left,
                    layout.bottom_screen.top, layout.bottom_screen.right,
                    layout.bottom_screen.bottom);
}

bool EmuWindow_Switch::HasPresentedSoftwareFrame() const {
    return software_frame_has_rgb;
}

void EmuWindow_Switch::PresentLoadingFrame(u32 tick) {
    if (Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::Deko3D ||
        Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::OpenGL) {
        if (tick == 0) {
            AppendLogFormat(nullptr, "android-flow stage=switch-framebuffer.skip reason=%s",
                            Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::Deko3D
                                ? "deko3d"
                                : "opengl");
        }
        return;
    }

    if (!InitializeFramebuffer()) {
        return;
    }

    u32 stride = 0;
    u8* frame = Platform::BeginFramebuffer(framebuffer, &stride);
    if (frame == nullptr) {
        return;
    }

    FillWaitingPattern(frame, stride, tick);
    Platform::EndFramebuffer(framebuffer);

    if (tick == 0 || (tick % 1024) == 0) {
        AppendLogFormat(nullptr, "android-flow stage=switch-frame.waiting tick=%u", tick);
    }
}

} // namespace Azahar::Switch
