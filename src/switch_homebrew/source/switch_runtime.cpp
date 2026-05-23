// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "switch_runtime.h"

#include "emulator_bootstrap.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deko3d.hpp>
#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>
#endif
#include <memory>
#include <switch.h>

namespace Azahar::Switch::Platform {
namespace {

constexpr std::uint32_t DekoPresentOk = 0;
constexpr std::uint32_t DekoErrorDevice = 1;
constexpr std::uint32_t DekoErrorQueue = 2;
constexpr std::uint32_t DekoErrorCommandBuffer = 3;
constexpr std::uint32_t DekoErrorCommandMemory = 4;
constexpr std::uint32_t DekoErrorFramebuffer = 5;
constexpr std::uint32_t DekoErrorSwapchain = 6;
constexpr std::uint32_t DekoErrorSourceImage = 7;
constexpr std::uint32_t DekoErrorStaging = 8;
constexpr std::uint32_t DekoErrorAcquire = 9;

constexpr std::uint32_t ApmFastLoadDockedConfig = 0x92220009;
constexpr std::uint32_t ApmFastLoadHandheldConfig = 0x9222000A;
constexpr std::uint32_t SwitchCoreThreadPriority = 0x2B;

struct ApmBoostState {
    bool initialized{};
    bool active{};
    bool applet_active{};
    std::uint32_t normal_config{};
    std::uint32_t boost_config{};
};

ApmBoostState apm_boost_state;

struct ThreadPriorityBoostState {
    bool active{};
    std::uint32_t original_priority{};
};

ThreadPriorityBoostState thread_priority_boost_state;

CpuBoostResult SetApmFastLoad(bool enabled) {
    if (!enabled) {
        std::uint32_t result = 0;
        if (apm_boost_state.active) {
            const std::uint32_t normal_result = static_cast<std::uint32_t>(
                apmSetPerformanceConfiguration(ApmPerformanceMode_Normal,
                                               apm_boost_state.normal_config));
            const std::uint32_t boost_result = static_cast<std::uint32_t>(
                apmSetPerformanceConfiguration(ApmPerformanceMode_Boost,
                                               apm_boost_state.boost_config));
            result = normal_result != 0 ? normal_result : boost_result;
            apm_boost_state.active = false;
        }
        if (apm_boost_state.initialized) {
            apmExit();
            apm_boost_state.initialized = false;
        }
        return {result, CpuBoostMethod::ApmPerformanceConfig};
    }

    std::uint32_t result = static_cast<std::uint32_t>(apmInitialize());
    if (result != 0) {
        return {result, CpuBoostMethod::ApmPerformanceConfig};
    }
    apm_boost_state.initialized = true;

    result = static_cast<std::uint32_t>(
        apmGetPerformanceConfiguration(ApmPerformanceMode_Normal,
                                       &apm_boost_state.normal_config));
    if (result != 0) {
        return {result, CpuBoostMethod::ApmPerformanceConfig};
    }

    result = static_cast<std::uint32_t>(
        apmGetPerformanceConfiguration(ApmPerformanceMode_Boost,
                                       &apm_boost_state.boost_config));
    if (result != 0) {
        return {result, CpuBoostMethod::ApmPerformanceConfig};
    }

    const std::uint32_t normal_result = static_cast<std::uint32_t>(
        apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, ApmFastLoadHandheldConfig));
    const std::uint32_t boost_result = static_cast<std::uint32_t>(
        apmSetPerformanceConfiguration(ApmPerformanceMode_Boost, ApmFastLoadDockedConfig));
    result = normal_result != 0 ? normal_result : boost_result;
    apm_boost_state.active = result == 0;
    return {result, CpuBoostMethod::ApmPerformanceConfig};
}

#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
constexpr std::uint32_t GlPresentOk = 0;
constexpr std::uint32_t GlErrorDisplay = 0x1001;
constexpr std::uint32_t GlErrorInitialize = 0x1002;
constexpr std::uint32_t GlErrorBindApi = 0x1003;
constexpr std::uint32_t GlErrorChooseConfig = 0x1004;
constexpr std::uint32_t GlErrorSurface = 0x1005;
constexpr std::uint32_t GlErrorContext = 0x1006;
constexpr std::uint32_t GlErrorMakeCurrent = 0x1007;
constexpr std::uint32_t GlErrorGlad = 0x1008;
constexpr std::uint32_t GlErrorSwap = 0x1009;

#ifdef USING_GLES
constexpr EGLenum SwitchEglApi = EGL_OPENGL_ES_API;
constexpr EGLint SwitchEglRenderableType = EGL_OPENGL_ES3_BIT_KHR;
constexpr const char* SwitchEglApiName = "opengles3";
constexpr EGLint SwitchEglContextAttributes[] = {
    EGL_CONTEXT_CLIENT_VERSION,
    3,
    EGL_NONE,
};
#else
constexpr EGLenum SwitchEglApi = EGL_OPENGL_API;
constexpr EGLint SwitchEglRenderableType = EGL_OPENGL_BIT;
constexpr const char* SwitchEglApiName = "opengl-core";
constexpr EGLint SwitchEglContextAttributes[] = {
    EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
    EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
    EGL_CONTEXT_MAJOR_VERSION_KHR,
    4,
    EGL_CONTEXT_MINOR_VERSION_KHR,
    3,
    EGL_NONE,
};
#endif
#endif

constexpr std::uint32_t NumDekoFramebuffers = 2;
constexpr std::uint32_t DekoCommandMemorySize = 0x10000;

PadState frame_pump_pad{};
bool frame_pump_input_ready = false;

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

struct DekoMemBlock {
    dk::MemBlock block{};
    std::uint32_t size = 0;

    ~DekoMemBlock() {
        Destroy();
    }

    DekoMemBlock() = default;
    DekoMemBlock(const DekoMemBlock&) = delete;
    DekoMemBlock& operator=(const DekoMemBlock&) = delete;

    bool Create(dk::Device device, std::uint32_t requested_size, std::uint32_t flags) {
        Destroy();
        size = AlignUp(std::max(requested_size, static_cast<std::uint32_t>(DK_MEMBLOCK_ALIGNMENT)),
                       static_cast<std::uint32_t>(DK_MEMBLOCK_ALIGNMENT));
        block = dk::MemBlockMaker{device, size}.setFlags(flags).create();
        return static_cast<bool>(block);
    }

    void Destroy() {
        if (block) {
            block.destroy();
        }
        size = 0;
    }

    void* CpuAddr() {
        return block ? block.getCpuAddr() : nullptr;
    }

    DkGpuAddr GpuAddr() {
        return block ? block.getGpuAddr() : 0;
    }
};

struct DekoSourceImage {
    dk::Image image{};
    DekoMemBlock image_memory{};
    DekoMemBlock staging_memory{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool ready = false;

    bool Ensure(dk::Device device, dk::Queue queue, std::uint32_t new_width,
                std::uint32_t new_height) {
        if (width == new_width && height == new_height && ready && staging_memory.block) {
            return true;
        }

        queue.waitIdle();
        image = {};
        image_memory.Destroy();
        staging_memory.Destroy();
        width = new_width;
        height = new_height;
        ready = false;

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(width, height)
            .initialize(layout);

        if (!image_memory.Create(device, layout.getSize(),
                                 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                     DkMemBlockFlags_Image)) {
            return false;
        }
        image.initialize(layout, image_memory.block, 0);

        const std::uint32_t staging_size = width * height * 4;
        ready = staging_memory.Create(device, staging_size,
                                      DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        return ready;
    }

    bool Upload(const DekoSoftwareScreen& screen) {
        const std::uint32_t byte_size = width * height * 4;
        void* dst = staging_memory.CpuAddr();
        if (dst == nullptr || screen.pixels == nullptr) {
            return false;
        }
        std::memcpy(dst, screen.pixels, byte_size);
        staging_memory.block.flushCpuCache(0, byte_size);
        return true;
    }
};

struct DekoPresenter {
    dk::UniqueDevice device{};
    dk::UniqueQueue queue{};
    dk::UniqueCmdBuf command_buffer{};
    dk::UniqueSwapchain swapchain{};
    DekoMemBlock command_memory{};
    std::array<dk::Image, NumDekoFramebuffers> framebuffers{};
    std::array<DekoMemBlock, NumDekoFramebuffers> framebuffer_memory{};
    DekoSourceImage top{};
    DekoSourceImage bottom{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool Initialize(std::uint32_t framebuffer_width, std::uint32_t framebuffer_height,
                    std::uint32_t* error_out) {
        width = framebuffer_width;
        height = framebuffer_height;

        device = dk::DeviceMaker{}.create();
        if (!device) {
            SetError(error_out, DekoErrorDevice);
            return false;
        }

        queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();
        if (!queue) {
            SetError(error_out, DekoErrorQueue);
            return false;
        }

        command_buffer = dk::CmdBufMaker{device}.create();
        if (!command_buffer) {
            SetError(error_out, DekoErrorCommandBuffer);
            return false;
        }
        if (!command_memory.Create(device, DekoCommandMemorySize,
                                   DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)) {
            SetError(error_out, DekoErrorCommandMemory);
            return false;
        }

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
                      DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(width, height)
            .initialize(layout);

        std::array<DkImage const*, NumDekoFramebuffers> images{};
        for (std::uint32_t i = 0; i < NumDekoFramebuffers; ++i) {
            if (!framebuffer_memory[i].Create(device, layout.getSize(),
                                              DkMemBlockFlags_CpuUncached |
                                                  DkMemBlockFlags_GpuCached |
                                                  DkMemBlockFlags_Image)) {
                SetError(error_out, DekoErrorFramebuffer);
                return false;
            }
            framebuffers[i].initialize(layout, framebuffer_memory[i].block, 0);
            images[i] = &framebuffers[i];
        }

        swapchain = dk::SwapchainMaker{device, nwindowGetDefault(), images}.create();
        if (!swapchain) {
            SetError(error_out, DekoErrorSwapchain);
            return false;
        }

        SetError(error_out, DekoPresentOk);
        return true;
    }

    bool Present(const DekoSoftwareScreen& top_screen, const DekoSoftwareScreen& bottom_screen,
                 std::uint32_t* error_out) {
        if (!PrepareSource(top, top_screen, error_out) ||
            !PrepareSource(bottom, bottom_screen, error_out)) {
            return false;
        }

        const int slot = queue.acquireImage(swapchain);
        if (slot < 0 || slot >= static_cast<int>(NumDekoFramebuffers)) {
            SetError(error_out, DekoErrorAcquire);
            return false;
        }

        command_buffer.clear();
        command_buffer.addMemory(command_memory.block, 0, command_memory.size);

        dk::ImageView framebuffer_view{framebuffers[slot]};
        const std::array<DkImageView const*, 1> render_targets{&framebuffer_view};
        command_buffer.bindRenderTargets(render_targets);
        command_buffer.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

        BlitSource(top, top_screen, framebuffer_view);
        BlitSource(bottom, bottom_screen, framebuffer_view);

        queue.submitCommands(command_buffer.finishList());
        queue.presentImage(swapchain, slot);
        SetError(error_out, DekoPresentOk);
        return true;
    }

    static void SetError(std::uint32_t* error_out, std::uint32_t error) {
        if (error_out != nullptr) {
            *error_out = error;
        }
    }

    bool PrepareSource(DekoSourceImage& source, const DekoSoftwareScreen& screen,
                       std::uint32_t* error_out) {
        if (!screen.enabled) {
            return true;
        }
        if (!source.Ensure(device, queue, screen.width, screen.height)) {
            SetError(error_out, DekoErrorSourceImage);
            return false;
        }
        if (!source.Upload(screen)) {
            SetError(error_out, DekoErrorStaging);
            return false;
        }
        return true;
    }

    void BlitSource(DekoSourceImage& source, const DekoSoftwareScreen& screen,
                    const dk::ImageView& framebuffer_view) {
        if (!screen.enabled || screen.dst.right <= screen.dst.left ||
            screen.dst.bottom <= screen.dst.top) {
            return;
        }

        const DkCopyBuf upload_buffer{source.staging_memory.GpuAddr(), 0, 0};
        const DkImageRect source_rect{0, 0, 0, source.width, source.height, 1};
        dk::ImageView source_view{source.image};
        command_buffer.copyBufferToImage(upload_buffer, source_view, source_rect);

        const DkImageRect dst_rect{screen.dst.left,
                                   screen.dst.top,
                                   0,
                                   screen.dst.right - screen.dst.left,
                                   screen.dst.bottom - screen.dst.top,
                                   1};
        command_buffer.blitImage(source_view, source_rect, framebuffer_view, dst_rect,
                                 DkBlitFlag_FilterNearest | DkBlitFlag_ModeBlit);
    }
};

#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
struct OpenGLContext {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    bool bindings_loaded = false;
    bool owns_display = true;
    bool owns_surface = true;
    bool supports_pbuffer = false;

    bool Initialize(std::uint32_t width, std::uint32_t height, std::uint32_t* error_out) {
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.get-display begin");
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.get-display display=%p",
                        display);
        if (display == EGL_NO_DISPLAY) {
            SetError(error_out, GlErrorDisplay);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.egl.get-display result=%08X egl=%04X",
                            GlErrorDisplay, eglGetError());
            return false;
        }

        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.initialize begin");
        if (eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
            SetError(error_out, GlErrorInitialize);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.egl.initialize result=%08X egl=%04X",
                            GlErrorInitialize, eglGetError());
            return false;
        }
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.initialize result=ok");

        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.bind-api begin api=%s",
                        SwitchEglApiName);
        if (eglBindAPI(SwitchEglApi) != EGL_TRUE) {
            SetError(error_out, GlErrorBindApi);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.egl.bind-api result=%08X egl=%04X",
                            GlErrorBindApi, eglGetError());
            return false;
        }
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.bind-api result=ok");

        const EGLint shared_surface_attributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, SwitchEglRenderableType,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_STENCIL_SIZE, 8,
            EGL_NONE,
        };
        const EGLint window_surface_attributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, SwitchEglRenderableType,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_STENCIL_SIZE, 8,
            EGL_NONE,
        };

        EGLint config_count = 0;
        config = nullptr;
        AppendLogFormat(nullptr,
                        "android-flow stage=switch-opengl.egl.choose-config begin surface=pbuffer");
        if (eglChooseConfig(display, shared_surface_attributes, &config, 1, &config_count) ==
                EGL_TRUE &&
            config_count > 0) {
            supports_pbuffer = true;
        } else {
            const EGLint pbuffer_config_error = eglGetError();
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.egl.choose-config fallback=window result=%08X egl=%04X count=%d",
                            GlErrorChooseConfig, pbuffer_config_error, config_count);
            config = nullptr;
            config_count = 0;
        }

        if (config == nullptr &&
            (eglChooseConfig(display, window_surface_attributes, &config, 1, &config_count) !=
                 EGL_TRUE ||
             config_count == 0)) {
            SetError(error_out, GlErrorChooseConfig);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.egl.choose-config result=%08X egl=%04X count=%d",
                            GlErrorChooseConfig, eglGetError(), config_count);
            return false;
        }
        AppendLogFormat(nullptr,
                        "android-flow stage=switch-opengl.egl.choose-config result=ok count=%d pbuffer=%u",
                        config_count, supports_pbuffer ? 1 : 0);

        NWindow* window = nwindowGetDefault();
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.window window=%p", window);
        nwindowSetDimensions(window, width, height);
        AppendLogFormat(nullptr,
                        "android-flow stage=switch-opengl.egl.window-dimensions width=%u height=%u",
                        width, height);
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.create-surface begin");
        surface = eglCreateWindowSurface(display, config, window, nullptr);
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.create-surface surface=%p",
                        surface);
        if (surface == EGL_NO_SURFACE) {
            SetError(error_out, GlErrorSurface);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.egl.create-surface result=%08X egl=%04X",
                            GlErrorSurface, eglGetError());
            return false;
        }

        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.create-context begin");
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, SwitchEglContextAttributes);
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.create-context context=%p",
                        context);
        if (context == EGL_NO_CONTEXT) {
            SetError(error_out, GlErrorContext);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.egl.create-context result=%08X egl=%04X",
                            GlErrorContext, eglGetError());
            return false;
        }

        if (!MakeCurrent(error_out)) {
            return false;
        }

        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.swap-interval begin");
        eglSwapInterval(display, 0);
        SetError(error_out, GlPresentOk);
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.egl.ready");
        return true;
    }

    bool InitializeShared(OpenGLContext& parent, std::uint32_t* error_out) {
        if (parent.display == EGL_NO_DISPLAY || parent.context == EGL_NO_CONTEXT ||
            parent.config == nullptr) {
            SetError(error_out, GlErrorContext);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.shared.parent result=%08X",
                            GlErrorContext);
            return false;
        }
        if (!parent.supports_pbuffer) {
            SetError(error_out, GlErrorSurface);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.shared.disabled reason=no-pbuffer result=%08X",
                            GlErrorSurface);
            return false;
        }

        display = parent.display;
        config = parent.config;
        owns_display = false;
        supports_pbuffer = true;

        const EGLint pbuffer_attributes[] = {
            EGL_WIDTH, 1,
            EGL_HEIGHT, 1,
            EGL_NONE,
        };

        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.shared.create-surface begin");
        surface = eglCreatePbufferSurface(display, config, pbuffer_attributes);
        if (surface == EGL_NO_SURFACE) {
            SetError(error_out, GlErrorSurface);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.shared.create-surface result=%08X egl=%04X",
                            GlErrorSurface, eglGetError());
            return false;
        }

        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.shared.create-context begin");
        context = eglCreateContext(display, config, parent.context, SwitchEglContextAttributes);
        if (context == EGL_NO_CONTEXT) {
            SetError(error_out, GlErrorContext);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.shared.create-context result=%08X egl=%04X",
                            GlErrorContext, eglGetError());
            return false;
        }

        SetError(error_out, GlPresentOk);
        AppendLogFormat(nullptr, "android-flow stage=switch-opengl.shared.ready");
        return true;
    }

    bool MakeCurrent(std::uint32_t* error_out) {
        if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
            SetError(error_out, GlErrorMakeCurrent);
            AppendLogFormat(nullptr,
                            "android-flow stage=switch-opengl.egl.make-current result=%08X egl=%04X",
                            GlErrorMakeCurrent, eglGetError());
            return false;
        }

        if (!bindings_loaded) {
            AppendLogFormat(nullptr, "android-flow stage=switch-opengl.glad.load begin");
#ifdef USING_GLES
            if (!gladLoadGLES2Loader((GLADloadproc)eglGetProcAddress)) {
#else
            if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress)) {
#endif
                SetError(error_out, GlErrorGlad);
                AppendLogFormat(nullptr,
                                "android-flow stage=switch-opengl.glad.load result=%08X",
                                GlErrorGlad);
                return false;
            }
            bindings_loaded = true;
            AppendLogFormat(nullptr, "android-flow stage=switch-opengl.glad.load result=ok");
        }

        SetError(error_out, GlPresentOk);
        return true;
    }

    void ClearCurrent() {
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    }

    bool Swap(std::uint32_t* error_out) {
        if (eglSwapBuffers(display, surface) != EGL_TRUE) {
            SetError(error_out, GlErrorSwap);
            return false;
        }
        SetError(error_out, GlPresentOk);
        return true;
    }

    ~OpenGLContext() {
        if (display != EGL_NO_DISPLAY) {
            ClearCurrent();
            if (context != EGL_NO_CONTEXT) {
                eglDestroyContext(display, context);
            }
            if (owns_surface && surface != EGL_NO_SURFACE) {
                eglDestroySurface(display, surface);
            }
            if (owns_display) {
                eglTerminate(display);
            }
            eglReleaseThread();
        }
    }

    static void SetError(std::uint32_t* error_out, std::uint32_t error) {
        if (error_out != nullptr) {
            *error_out = error;
        }
    }
};
#endif

} // namespace

void* CreateLinearFramebuffer(std::uint32_t width, std::uint32_t height, std::uint32_t* error_out) {
    auto* framebuffer = new Framebuffer{};
    Result result = framebufferCreate(framebuffer, nwindowGetDefault(), width, height,
                                      PIXEL_FORMAT_RGBA_8888, 2);
    if (R_SUCCEEDED(result)) {
        result = framebufferMakeLinear(framebuffer);
    }
    if (error_out != nullptr) {
        *error_out = result;
    }
    if (R_FAILED(result)) {
        framebufferClose(framebuffer);
        delete framebuffer;
        return nullptr;
    }
    return framebuffer;
}

void DestroyFramebuffer(void* handle) {
    if (handle == nullptr) {
        return;
    }
    auto* framebuffer = static_cast<Framebuffer*>(handle);
    framebufferClose(framebuffer);
    delete framebuffer;
}

std::uint8_t* BeginFramebuffer(void* handle, std::uint32_t* stride_out) {
    if (handle == nullptr) {
        return nullptr;
    }
    return static_cast<std::uint8_t*>(
        framebufferBegin(static_cast<Framebuffer*>(handle), stride_out));
}

void EndFramebuffer(void* handle) {
    if (handle == nullptr) {
        return;
    }
    framebufferEnd(static_cast<Framebuffer*>(handle));
}

void* CreateDekoPresenter(std::uint32_t width, std::uint32_t height, std::uint32_t* error_out) {
    auto presenter = std::make_unique<DekoPresenter>();
    if (!presenter->Initialize(width, height, error_out)) {
        return nullptr;
    }
    return presenter.release();
}

void DestroyDekoPresenter(void* handle) {
    delete static_cast<DekoPresenter*>(handle);
}

bool PresentDekoSoftwareFrame(void* handle, const DekoSoftwareScreen& top,
                              const DekoSoftwareScreen& bottom, std::uint32_t* error_out) {
    if (handle == nullptr) {
        DekoPresenter::SetError(error_out, DekoErrorDevice);
        return false;
    }
    return static_cast<DekoPresenter*>(handle)->Present(top, bottom, error_out);
}

#ifdef AZAHAR_SWITCH_OPENGL_SPIKE
void* CreateOpenGLContext(std::uint32_t width, std::uint32_t height, std::uint32_t* error_out) {
    auto context = std::make_unique<OpenGLContext>();
    if (!context->Initialize(width, height, error_out)) {
        return nullptr;
    }
    return context.release();
}

void* CreateOpenGLSharedContext(void* parent_handle, std::uint32_t* error_out) {
    if (parent_handle == nullptr) {
        OpenGLContext::SetError(error_out, GlErrorContext);
        return nullptr;
    }

    auto context = std::make_unique<OpenGLContext>();
    auto& parent = *static_cast<OpenGLContext*>(parent_handle);
    if (!context->InitializeShared(parent, error_out)) {
        return nullptr;
    }
    return context.release();
}

void DestroyOpenGLContext(void* handle) {
    delete static_cast<OpenGLContext*>(handle);
}

bool MakeOpenGLCurrent(void* handle, std::uint32_t* error_out) {
    if (handle == nullptr) {
        OpenGLContext::SetError(error_out, GlErrorContext);
        return false;
    }
    return static_cast<OpenGLContext*>(handle)->MakeCurrent(error_out);
}

void ClearOpenGLCurrent(void* handle) {
    if (handle != nullptr) {
        static_cast<OpenGLContext*>(handle)->ClearCurrent();
    }
}

bool SwapOpenGLBuffers(void* handle, std::uint32_t* error_out) {
    if (handle == nullptr) {
        OpenGLContext::SetError(error_out, GlErrorContext);
        return false;
    }
    return static_cast<OpenGLContext*>(handle)->Swap(error_out);
}
#endif

void InitializeFramePumpInput() {
    padInitializeDefault(&frame_pump_pad);
    frame_pump_input_ready = true;
}

bool FramePumpStopRequested() {
    if (!frame_pump_input_ready) {
        InitializeFramePumpInput();
    }
    padUpdate(&frame_pump_pad);
    const u64 held_buttons = padGetButtons(&frame_pump_pad);
    return (held_buttons & HidNpadButton_Plus) != 0 &&
           (held_buttons & HidNpadButton_Minus) != 0;
}

bool AppletMainLoop() {
    return appletMainLoop();
}

std::uint32_t GetHosVersion() {
    return hosversionGet();
}

CpuBoostResult SetCpuBoost(bool enabled) {
    if (!enabled) {
        std::uint32_t applet_result = 0;
        if (apm_boost_state.applet_active) {
            applet_result = static_cast<std::uint32_t>(
                appletSetCpuBoostMode(ApmCpuBoostMode_Normal));
            apm_boost_state.applet_active = false;
        }
        const CpuBoostResult apm_result = SetApmFastLoad(false);
        return {applet_result != 0 ? applet_result : apm_result.result,
                applet_result != 0 ? CpuBoostMethod::Applet : apm_result.method};
    }

    if (hosversionAtLeast(7, 0, 0)) {
        const std::uint32_t applet_result = static_cast<std::uint32_t>(
            appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad));
        if (applet_result == 0) {
            apm_boost_state.applet_active = true;
            return {0, CpuBoostMethod::Applet};
        }
    }

    return SetApmFastLoad(true);
}

ThreadPriorityBoostResult SetThreadPriorityBoost(bool enabled) {
    if (!enabled) {
        if (!thread_priority_boost_state.active) {
            return {0, 0, 0, 0, false};
        }
        const std::uint32_t set_result = static_cast<std::uint32_t>(
            svcSetThreadPriority(CUR_THREAD_HANDLE, thread_priority_boost_state.original_priority));
        const std::uint32_t original_priority = thread_priority_boost_state.original_priority;
        thread_priority_boost_state.active = false;
        return {0, set_result, original_priority, original_priority, false};
    }

    s32 original_priority = 0;
    const std::uint32_t get_result =
        static_cast<std::uint32_t>(svcGetThreadPriority(&original_priority, CUR_THREAD_HANDLE));
    if (get_result != 0) {
        return {get_result, 0, 0, SwitchCoreThreadPriority, false};
    }

    const std::uint32_t original = static_cast<std::uint32_t>(original_priority);
    const std::uint32_t requested =
        original > SwitchCoreThreadPriority ? SwitchCoreThreadPriority : original;
    if (requested == original) {
        return {0, 0, original, requested, false};
    }

    const std::uint32_t set_result =
        static_cast<std::uint32_t>(svcSetThreadPriority(CUR_THREAD_HANDLE, requested));
    if (set_result == 0) {
        thread_priority_boost_state.active = true;
        thread_priority_boost_state.original_priority = original;
    }
    return {0, set_result, original, requested, thread_priority_boost_state.active};
}

void SuspendConsole() {
    consoleExit(nullptr);
}

} // namespace Azahar::Switch::Platform

namespace Azahar::Switch {

// Exposes libnx heap stats to non-Switch-header callers (the citra/azahar
// type system conflicts with <switch.h>, so anything outside this file uses
// this helper). On query failure the outputs are left at 0.
void QueryHeapInfo(unsigned long long* total_bytes, unsigned long long* used_bytes) {
    // mallinfo() caused devkitA64 builds to refuse to boot — falling back to
    // the OS-level reservation (constant, so the log is mostly useless for
    // leak hunting, but harmless). A future change should override operator
    // new/delete and maintain an atomic counter instead.
    constexpr Handle kCurrentProcess = CUR_PROCESS_HANDLE;
    if (total_bytes != nullptr) {
        u64 value = 0;
        if (R_SUCCEEDED(svcGetInfo(&value, InfoType_TotalMemorySize, kCurrentProcess, 0))) {
            *total_bytes = static_cast<unsigned long long>(value);
        } else {
            *total_bytes = 0;
        }
    }
    if (used_bytes != nullptr) {
        u64 value = 0;
        if (R_SUCCEEDED(svcGetInfo(&value, InfoType_UsedMemorySize, kCurrentProcess, 0))) {
            *used_bytes = static_cast<unsigned long long>(value);
        } else {
            *used_bytes = 0;
        }
    }
}

} // namespace Azahar::Switch
