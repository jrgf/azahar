// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

#include "core/frontend/emu_window.h"

#include <memory>

namespace SwRenderer {
struct ScreenInfo;
}

namespace Azahar::Switch {

class EmuWindow_Switch final : public Frontend::EmuWindow {
public:
    explicit EmuWindow_Switch(unsigned width = 1280, unsigned height = 720);
    ~EmuWindow_Switch() override;

    bool IsGLES() override;
    void SwapBuffers() override;
    void MakeCurrent() override;
    void DoneCurrent() override;
    std::unique_ptr<Frontend::GraphicsContext> CreateSharedContext() const override;
    void PollEvents() override;
    void SetFramebufferSize(unsigned width, unsigned height);
    void PresentLoadingFrame(u32 tick);
    [[nodiscard]] bool HasPresentedSoftwareFrame() const;

private:
    bool InitializeFramebuffer();
    bool EnsureOpenGLPresenterContext();
    bool MakeOpenGLPresenterCurrent();
    void ProcessSwitchTouch();
    void PresentSoftwareFrame();
    bool PresentDekoFrame(const SwRenderer::ScreenInfo& top_screen,
                          const SwRenderer::ScreenInfo& bottom_screen,
                          const Layout::FramebufferLayout& layout);
    void BlitScreen(u8* frame, u32 stride, const SwRenderer::ScreenInfo& info,
                    const Common::Rectangle<u32>& rect);

    void* framebuffer = nullptr;
    void* deko_presenter = nullptr;
    void* gl_context = nullptr;
    void* gl_core_context = nullptr;
    bool framebuffer_ready = false;
    bool framebuffer_failed = false;
    bool deko_ready = false;
    bool deko_failed = false;
    bool gl_ready = false;
    bool gl_core_ready = false;
    bool gl_failed = false;
    u32 present_count = 0;
    u32 deko_present_count = 0;
    u32 gl_present_count = 0;
    bool software_frame_has_rgb = false;
    bool switch_touch_active = false;
};

} // namespace Azahar::Switch
