// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

#include <cstdint>

namespace Azahar::Switch::Platform {

struct DekoScreenRect {
    std::uint32_t left;
    std::uint32_t top;
    std::uint32_t right;
    std::uint32_t bottom;
};

struct DekoSoftwareScreen {
    const std::uint8_t* pixels;
    std::uint32_t width;
    std::uint32_t height;
    DekoScreenRect dst;
    bool enabled;
};

void* CreateLinearFramebuffer(std::uint32_t width, std::uint32_t height, std::uint32_t* error_out);
void DestroyFramebuffer(void* handle);
std::uint8_t* BeginFramebuffer(void* handle, std::uint32_t* stride_out);
void EndFramebuffer(void* handle);

void* CreateDekoPresenter(std::uint32_t width, std::uint32_t height, std::uint32_t* error_out);
void DestroyDekoPresenter(void* handle);
bool PresentDekoSoftwareFrame(void* handle, const DekoSoftwareScreen& top,
                              const DekoSoftwareScreen& bottom, std::uint32_t* error_out);

#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
void* CreateOpenGLContext(std::uint32_t width, std::uint32_t height, std::uint32_t* error_out);
void* CreateOpenGLSharedContext(void* parent_handle, std::uint32_t* error_out);
void DestroyOpenGLContext(void* handle);
bool MakeOpenGLCurrent(void* handle, std::uint32_t* error_out);
void ClearOpenGLCurrent(void* handle);
bool SwapOpenGLBuffers(void* handle, std::uint32_t* error_out);
#endif

void InitializeFramePumpInput();
bool FramePumpStopRequested();
bool AppletMainLoop();
std::uint32_t GetHosVersion();

enum class CpuBoostMethod : std::uint32_t {
    None = 0,
    Applet = 1,
    ApmPerformanceConfig = 2,
};

struct CpuBoostResult {
    std::uint32_t result;
    CpuBoostMethod method;
};

struct ThreadPriorityBoostResult {
    std::uint32_t get_result;
    std::uint32_t set_result;
    std::uint32_t original_priority;
    std::uint32_t requested_priority;
    bool active;
};

CpuBoostResult SetCpuBoost(bool enabled);
ThreadPriorityBoostResult SetThreadPriorityBoost(bool enabled);
void SuspendConsole();

} // namespace Azahar::Switch::Platform
