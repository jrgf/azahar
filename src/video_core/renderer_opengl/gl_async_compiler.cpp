// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "video_core/renderer_opengl/gl_async_compiler.h"

#ifdef __SWITCH__

#include <algorithm>
#include <chrono>
#include <string_view>
#include <utility>
#include <EGL/egl.h>

namespace Azahar::Switch {
bool AppendLogFormat(int* error_out, const char* format, ...);
}

namespace OpenGL {

struct AsyncShaderCompiler::Job {
    std::uint64_t config_hash;
    GLenum stage_type;
    std::string source;
    Callback on_ready;
};

namespace {

constexpr std::string_view kEnvLogStage = "android-flow stage=opengl.async.compiler.init";

bool HasSurfacelessExtension(EGLDisplay display) {
    const char* exts = eglQueryString(display, EGL_EXTENSIONS);
    if (exts == nullptr) {
        return false;
    }
    return std::string_view{exts}.find("EGL_KHR_surfaceless_context") != std::string_view::npos;
}

EGLConfig FindMainConfig(EGLDisplay display, EGLContext main_ctx) {
    EGLint config_id = 0;
    if (eglQueryContext(display, main_ctx, EGL_CONFIG_ID, &config_id) != EGL_TRUE ||
        config_id == 0) {
        return nullptr;
    }
    const EGLint filter[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (eglChooseConfig(display, filter, &cfg, 1, &n) != EGL_TRUE || n == 0) {
        return nullptr;
    }
    return cfg;
}

GLuint CompileStage(GLenum type, const std::string& source) {
    if (source.empty()) {
        return 0;
    }
    GLuint shader = glCreateShader(type);
    const char* ptr = source.c_str();
    glShaderSource(shader, 1, &ptr, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        // Capture the first chunk of the driver's compile log so we can see
        // what Mesa actually rejected — silent compile=0 is opaque otherwise.
        GLint log_length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        std::string info_log(static_cast<std::size_t>(std::min(log_length, 512)), '\0');
        if (!info_log.empty()) {
            glGetShaderInfoLog(shader, static_cast<GLsizei>(info_log.size()), nullptr,
                               info_log.data());
        }
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=opengl.async.compiler.compile-fail stage_type=0x%04X "
            "bytes=%zu log=\"%.480s\"",
            static_cast<unsigned>(type), source.size(), info_log.c_str());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

std::unique_ptr<AsyncShaderCompiler> AsyncShaderCompiler::Create() {
    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext main_ctx = eglGetCurrentContext();
    if (display == EGL_NO_DISPLAY || main_ctx == EGL_NO_CONTEXT) {
        Azahar::Switch::AppendLogFormat(nullptr, "%s result=no-egl-current",
                                        std::string{kEnvLogStage}.c_str());
        return nullptr;
    }

    if (!HasSurfacelessExtension(display)) {
        Azahar::Switch::AppendLogFormat(nullptr, "%s result=no-surfaceless-ext",
                                        std::string{kEnvLogStage}.c_str());
        return nullptr;
    }

    EGLConfig config = FindMainConfig(display, main_ctx);
    if (config == nullptr) {
        Azahar::Switch::AppendLogFormat(nullptr, "%s result=no-main-config egl=%04X",
                                        std::string{kEnvLogStage}.c_str(), eglGetError());
        return nullptr;
    }

    // The build flag USING_GLES is unreliable: on libretro Switch the binary
    // is built without USING_GLES (using desktop glad bindings) but RetroArch
    // hands us a GLES context at runtime. If we ignore that and ask for
    // OPENGL_CORE_PROFILE, Mesa will silently give us a desktop core context
    // — and then PICA's GLES-style shaders fail with "unrecognized layout
    // identifier `binding'". Query EGL's bound API to pick matching attribs.
    const EGLenum bound_api = eglQueryAPI();
    eglBindAPI(bound_api);

    const EGLint es_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                 EGL_CONTEXT_MAJOR_VERSION,  3,
                                 EGL_CONTEXT_MINOR_VERSION,  2,
                                 EGL_NONE};
    const EGLint gl_attribs[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_CONTEXT_MAJOR_VERSION,
                                 4,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 3,
                                 EGL_NONE};
    const EGLint* ctx_attribs =
        (bound_api == EGL_OPENGL_ES_API) ? es_attribs : gl_attribs;

    Azahar::Switch::AppendLogFormat(
        nullptr, "android-flow stage=opengl.async.compiler.init.api bound=0x%04X kind=%s",
        static_cast<unsigned>(bound_api),
        bound_api == EGL_OPENGL_ES_API ? "gles" : "gl");

    EGLContext shared = eglCreateContext(display, config, main_ctx, ctx_attribs);
    if (shared == EGL_NO_CONTEXT) {
        Azahar::Switch::AppendLogFormat(nullptr, "%s result=create-context-fail egl=%04X",
                                        std::string{kEnvLogStage}.c_str(), eglGetError());
        return nullptr;
    }

    Azahar::Switch::AppendLogFormat(nullptr, "%s result=ok shared_ctx=%p",
                                    std::string{kEnvLogStage}.c_str(), shared);

    return std::unique_ptr<AsyncShaderCompiler>(
        new AsyncShaderCompiler(static_cast<void*>(display), static_cast<void*>(shared)));
}

AsyncShaderCompiler::AsyncShaderCompiler(void* display, void* shared_ctx)
    : display_(display), shared_ctx_(shared_ctx) {
    worker_ = std::thread(&AsyncShaderCompiler::WorkerLoop, this);
}

AsyncShaderCompiler::~AsyncShaderCompiler() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (shared_ctx_ != nullptr && display_ != nullptr) {
        eglDestroyContext(static_cast<EGLDisplay>(display_),
                          static_cast<EGLContext>(shared_ctx_));
    }
}

void AsyncShaderCompiler::Enqueue(std::uint64_t config_hash, GLenum stage_type,
                                  std::string source, Callback on_ready) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_.load(std::memory_order_acquire)) {
            return;
        }
        if (!in_flight_.insert(config_hash).second) {
            // Dedup: drop silently.
            return;
        }
        queue_.push_back(
            Job{config_hash, stage_type, std::move(source), std::move(on_ready)});
        enqueued_count_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
}

void AsyncShaderCompiler::WorkerLoop() {
    EGLDisplay display = static_cast<EGLDisplay>(display_);
    EGLContext ctx = static_cast<EGLContext>(shared_ctx_);
    if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) != EGL_TRUE) {
        Azahar::Switch::AppendLogFormat(
            nullptr, "android-flow stage=opengl.async.compiler.worker.mkcurrent result=fail egl=%04X",
            eglGetError());
        return;
    }
    const auto* gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* glsl_version =
        reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=opengl.async.compiler.worker.mkcurrent result=ok ctx=%p "
        "gl=\"%s\" glsl=\"%s\"",
        ctx, gl_version != nullptr ? gl_version : "?",
        glsl_version != nullptr ? glsl_version : "?");

    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return shutdown_.load(std::memory_order_acquire) || !queue_.empty();
            });
            if (shutdown_.load(std::memory_order_acquire) && queue_.empty()) {
                break;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        const auto start = std::chrono::steady_clock::now();
        GLuint shader = CompileStage(job.stage_type, job.source);
        GLuint program = 0;
        GLint link_status = GL_FALSE;
        if (shader != 0) {
            program = glCreateProgram();
            glProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
            glAttachShader(program, shader);
            glLinkProgram(program);
            glGetProgramiv(program, GL_LINK_STATUS, &link_status);
            glDetachShader(program, shader);
            if (link_status != GL_TRUE) {
                glDeleteProgram(program);
                program = 0;
            }
        }
        if (shader != 0) glDeleteShader(shader);

        // Flush so the program is visible from the main context's command
        // stream. If hardware ever needs an explicit fence, add glFenceSync +
        // glClientWaitSync here.
        glFlush();

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();

        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=opengl.async.compiler.job config=%016llX stage=0x%04X "
            "elapsed-ms=%lld compile=%u link=%d prog=%u bytes=%zu",
            static_cast<unsigned long long>(job.config_hash),
            static_cast<unsigned>(job.stage_type), static_cast<long long>(elapsed_ms),
            shader != 0 ? 1U : 0U, link_status == GL_TRUE ? 1 : 0, program, job.source.size());

        completed_count_.fetch_add(1, std::memory_order_relaxed);

        if (job.on_ready) {
            job.on_ready(program);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            in_flight_.erase(job.config_hash);
        }
    }

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void RunAsyncCompilerSmokeTest() {
    static std::atomic<bool> ran{false};
    bool expected = false;
    if (!ran.compare_exchange_strong(expected, true)) {
        return;
    }

    auto compiler = AsyncShaderCompiler::Create();
    if (compiler == nullptr) {
        Azahar::Switch::AppendLogFormat(
            nullptr, "android-flow stage=opengl.async.compiler.smoke result=no-compiler");
        return;
    }

    constexpr const char* kSmokeFS =
        "#version 330 core\nout vec4 c; void main() { c = vec4(0.0); }\n";

    std::atomic<int> ready_count{0};
    constexpr int kJobCount = 5;
    for (int i = 0; i < kJobCount; ++i) {
        compiler->Enqueue(
            static_cast<std::uint64_t>(0xA110CA7E00000000ull + i), GL_FRAGMENT_SHADER, kSmokeFS,
            [&ready_count](GLuint program) {
                if (program != 0) {
                    ready_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }
    // Also try a duplicate hash — should be deduped, not double-fire.
    compiler->Enqueue(0xA110CA7E00000000ull, GL_FRAGMENT_SHADER, kSmokeFS,
                      [](GLuint) {});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (compiler->CompletedCount() < kJobCount &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=opengl.async.compiler.smoke enqueued=%llu completed=%llu ready=%d",
        static_cast<unsigned long long>(compiler->EnqueuedCount()),
        static_cast<unsigned long long>(compiler->CompletedCount()), ready_count.load());
}

} // namespace OpenGL

#endif
