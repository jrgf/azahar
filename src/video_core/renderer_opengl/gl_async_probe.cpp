// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_opengl/gl_async_probe.h"

#ifdef __SWITCH__
#include <atomic>
#include <string_view>
#include <thread>
#include <EGL/egl.h>
#include <glad/glad.h>

namespace Azahar::Switch {
bool AppendLogFormat(int* error_out, const char* format, ...);
}
#endif

namespace OpenGL {

#ifdef __SWITCH__
namespace {

std::atomic<bool> probe_ran{false};

constexpr const char* kProbeVS =
    "#version 330 core\n"
    "void main() { gl_Position = vec4(0.0); }\n";

constexpr const char* kProbeFS =
    "#version 330 core\n"
    "out vec4 frag_color;\n"
    "void main() { frag_color = vec4(0.0); }\n";

GLuint CompileProbeShader(GLenum type, const char* source, GLint* status_out) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, status_out);
    return shader;
}

} // namespace

void RunSharedContextProbe() {
    bool expected = false;
    if (!probe_ran.compare_exchange_strong(expected, true)) {
        return;
    }

    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext main_ctx = eglGetCurrentContext();
    if (display == EGL_NO_DISPLAY || main_ctx == EGL_NO_CONTEXT) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=opengl.async.probe.precheck result=no-egl-current display=%p ctx=%p",
            display, main_ctx);
        return;
    }

    EGLint main_config_id = 0;
    eglQueryContext(display, main_ctx, EGL_CONFIG_ID, &main_config_id);

    EGLint main_surface_type = 0;
    if (main_config_id != 0) {
        const EGLint filter[] = {EGL_CONFIG_ID, main_config_id, EGL_NONE};
        EGLConfig main_cfg = nullptr;
        EGLint n = 0;
        if (eglChooseConfig(display, filter, &main_cfg, 1, &n) == EGL_TRUE && n > 0) {
            eglGetConfigAttrib(display, main_cfg, EGL_SURFACE_TYPE, &main_surface_type);
        }
    }

    const char* exts = eglQueryString(display, EGL_EXTENSIONS);
    const bool has_surfaceless =
        exts != nullptr && std::string_view{exts}.find("EGL_KHR_surfaceless_context") !=
                               std::string_view::npos;

    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=opengl.async.probe.env main_config_id=%d main_surface_type=0x%04X "
        "surfaceless_ext=%u",
        main_config_id, main_surface_type, has_surfaceless ? 1U : 0U);

#ifdef USING_GLES
    constexpr EGLint kRenderableTypeBit = EGL_OPENGL_ES3_BIT_KHR;
#else
    constexpr EGLint kRenderableTypeBit = EGL_OPENGL_BIT;
#endif

    EGLConfig config = nullptr;
    bool config_is_pbuffer_capable = false;
    {
        const EGLint pbuffer_filter[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                         EGL_RENDERABLE_TYPE, kRenderableTypeBit,
                                         EGL_NONE};
        EGLint n = 0;
        if (eglChooseConfig(display, pbuffer_filter, &config, 1, &n) == EGL_TRUE && n > 0) {
            config_is_pbuffer_capable = true;
        }
    }
    if (config == nullptr && main_config_id != 0) {
        const EGLint filter[] = {EGL_CONFIG_ID, main_config_id, EGL_NONE};
        EGLint n = 0;
        eglChooseConfig(display, filter, &config, 1, &n);
    }
    if (config == nullptr) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=opengl.async.probe.choose-config result=fail egl=%04X",
            eglGetError());
        return;
    }

    EGLSurface pbuffer = EGL_NO_SURFACE;
    if (!has_surfaceless && config_is_pbuffer_capable) {
        const EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        pbuffer = eglCreatePbufferSurface(display, config, pbuffer_attribs);
        if (pbuffer == EGL_NO_SURFACE) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=opengl.async.probe.pbuffer result=fail egl=%04X",
                eglGetError());
            return;
        }
    } else if (!has_surfaceless && !config_is_pbuffer_capable) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=opengl.async.probe.surfaceless-required result=unsupported "
            "main_surface_type=0x%04X",
            main_surface_type);
        return;
    }

#ifdef USING_GLES
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
#else
    const EGLint ctx_attribs[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                  EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                  EGL_CONTEXT_MAJOR_VERSION,
                                  4,
                                  EGL_CONTEXT_MINOR_VERSION,
                                  3,
                                  EGL_NONE};
#endif
    EGLContext shared_ctx = eglCreateContext(display, config, main_ctx, ctx_attribs);
    if (shared_ctx == EGL_NO_CONTEXT) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=opengl.async.probe.create-context result=fail egl=%04X",
            eglGetError());
        eglDestroySurface(display, pbuffer);
        return;
    }

    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=opengl.async.probe.create-context result=ok "
        "pbuffer_config=%u surfaceless=%u ctx=%p",
        config_is_pbuffer_capable ? 1U : 0U, has_surfaceless ? 1U : 0U, shared_ctx);

    std::atomic<GLuint> program_handle{0};
    std::atomic<int> worker_make_current{0};
    std::atomic<int> worker_vs_status{0};
    std::atomic<int> worker_fs_status{0};
    std::atomic<int> worker_link_status{0};
    std::atomic<EGLint> worker_last_egl_error{EGL_SUCCESS};

    std::thread worker([&] {
        if (eglMakeCurrent(display, pbuffer, pbuffer, shared_ctx) != EGL_TRUE) {
            worker_last_egl_error.store(eglGetError());
            return;
        }
        worker_make_current.store(1);

        GLint vs_status = 0;
        GLuint vs = CompileProbeShader(GL_VERTEX_SHADER, kProbeVS, &vs_status);
        worker_vs_status.store(vs_status);

        GLint fs_status = 0;
        GLuint fs = CompileProbeShader(GL_FRAGMENT_SHADER, kProbeFS, &fs_status);
        worker_fs_status.store(fs_status);

        if (vs_status == GL_TRUE && fs_status == GL_TRUE) {
            GLuint prog = glCreateProgram();
            glAttachShader(prog, vs);
            glAttachShader(prog, fs);
            glLinkProgram(prog);

            GLint link_status = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &link_status);
            worker_link_status.store(link_status);
            if (link_status == GL_TRUE) {
                program_handle.store(prog);
            } else {
                glDeleteProgram(prog);
            }
        }

        glDeleteShader(vs);
        glDeleteShader(fs);
        glFlush();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    });
    worker.join();

    const GLuint prog = program_handle.load();
    const GLboolean visible_from_main = prog != 0 ? glIsProgram(prog) : GL_FALSE;

    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=opengl.async.probe.result mkcurrent=%d vs=%d fs=%d link=%d "
        "prog=%u visible-from-main=%u worker_egl_err=%04X",
        worker_make_current.load(), worker_vs_status.load(), worker_fs_status.load(),
        worker_link_status.load(), prog, visible_from_main == GL_TRUE ? 1U : 0U,
        worker_last_egl_error.load());

    if (prog != 0) {
        glDeleteProgram(prog);
    }
    eglDestroyContext(display, shared_ctx);
    eglDestroySurface(display, pbuffer);
}

#else

void RunSharedContextProbe() {}

#endif

} // namespace OpenGL
