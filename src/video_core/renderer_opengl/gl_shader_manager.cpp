// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#ifdef __SWITCH__
#include <atomic>
#include <chrono>
#endif
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>
#include <variant>
#include "common/hash.h"
#include "common/file_util.h"
#include "common/settings.h"
#include "core/frontend/emu_window.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/renderer_opengl/gl_async_compiler.h"
#include "video_core/renderer_opengl/gl_driver.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"
#include "video_core/renderer_opengl/gl_shader_disk_cache.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/glsl_shader_gen.h"
#include "video_core/shader/generator/profile.h"

using namespace Pica::Shader::Generator;
using Pica::Shader::FSConfig;

#ifdef __SWITCH__
namespace Azahar::Switch {
bool AppendLogFormat(int* error_out, const char* format, ...);
}
#endif

namespace OpenGL {

#ifdef __SWITCH__
namespace {
std::atomic<unsigned> switch_gl_shader_trace_logs{};
constexpr unsigned SwitchGlShaderTraceLogLimit = 64;

bool SwitchGlShaderReserveLog() {
    return switch_gl_shader_trace_logs.fetch_add(1, std::memory_order_relaxed) <
           SwitchGlShaderTraceLogLimit;
}

long long SwitchGlShaderElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}
} // namespace
#endif

static u64 GetUniqueIdentifier(const Pica::RegsInternal& regs, const ProgramCode& code) {
    std::size_t hash = 0;
    u64 regs_uid =
        Common::ComputeHash64(regs.reg_array.data(), Pica::RegsInternal::NUM_REGS * sizeof(u32));
    hash = Common::HashCombine(hash, regs_uid);

    if (code.size() > 0) {
        u64 code_uid = Common::ComputeHash64(code.data(), code.size() * sizeof(u32));
        hash = Common::HashCombine(hash, code_uid);
    }

    return hash;
}

static OGLProgram GeneratePrecompiledProgram(const ShaderDiskCacheDump& dump,
                                             const std::set<GLenum>& supported_formats,
                                             bool separable) {

    if (supported_formats.find(dump.binary_format) == supported_formats.end()) {
        LOG_INFO(Render_OpenGL, "Precompiled cache entry with unsupported format - removing");
        return {};
    }

    auto shader = OGLProgram();
    shader.handle = glCreateProgram();
    if (separable) {
        glProgramParameteri(shader.handle, GL_PROGRAM_SEPARABLE, GL_TRUE);
    }
    glProgramBinary(shader.handle, dump.binary_format, dump.binary.data(),
                    static_cast<GLsizei>(dump.binary.size()));

    GLint link_status{};
    glGetProgramiv(shader.handle, GL_LINK_STATUS, &link_status);
    if (link_status == GL_FALSE) {
        LOG_INFO(Render_OpenGL, "Precompiled cache rejected by the driver - removing");
        return {};
    }

    return shader;
}

static std::set<GLenum> GetSupportedFormats() {
    std::set<GLenum> supported_formats;

    GLint num_formats{};
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &num_formats);

    std::vector<GLint> formats(num_formats);
    glGetIntegerv(GL_PROGRAM_BINARY_FORMATS, formats.data());

    for (const GLint format : formats)
        supported_formats.insert(static_cast<GLenum>(format));
    return supported_formats;
}

static std::tuple<PicaVSConfig, Pica::ShaderSetup> BuildVSConfigFromRaw(
    const ShaderDiskCacheRaw& raw, const Driver& driver, bool accurate_mul) {
    Pica::ProgramCode program_code{};
    Pica::SwizzleData swizzle_data{};
    std::copy_n(raw.GetProgramCode().begin(), Pica::MAX_PROGRAM_CODE_LENGTH, program_code.begin());
    std::copy_n(raw.GetProgramCode().begin() + Pica::MAX_PROGRAM_CODE_LENGTH,
                Pica::MAX_SWIZZLE_DATA_LENGTH, swizzle_data.begin());
    Pica::ShaderSetup setup;
    setup.UpdateProgramCode(program_code);
    setup.UpdateSwizzleData(swizzle_data);

    return {PicaVSConfig{raw.GetRawShaderConfig(), setup}, setup};
}

/**
 * An object representing a shader program staging. It can be either a shader object or a program
 * object, depending on whether separable program is used.
 */
class OGLShaderStage {
public:
    explicit OGLShaderStage(bool separable) {
        if (separable) {
            shader_or_program = OGLProgram();
        } else {
            shader_or_program = OGLShader();
        }
    }

    OGLShaderStage(const OGLShaderStage&) = delete;
    OGLShaderStage& operator=(const OGLShaderStage&) = delete;

#ifdef __SWITCH__
    // std::atomic is not movable; implement move manually so the cache can
    // still emplace/move stages. Any in-flight async handle on `other` is
    // transferred into `*this`; an existing async handle on `*this` is freed.
    OGLShaderStage(OGLShaderStage&& other) noexcept
        : shader_or_program(std::move(other.shader_or_program)),
          async_handle(other.async_handle.exchange(0, std::memory_order_acq_rel)) {}

    OGLShaderStage& operator=(OGLShaderStage&& other) noexcept {
        if (this != &other) {
            shader_or_program = std::move(other.shader_or_program);
            const GLuint taken = other.async_handle.exchange(0, std::memory_order_acq_rel);
            const GLuint prev = async_handle.exchange(taken, std::memory_order_acq_rel);
            if (prev != 0) {
                glDeleteProgram(prev);
            }
        }
        return *this;
    }

    ~OGLShaderStage() {
        const GLuint h = async_handle.load(std::memory_order_acquire);
        if (h != 0) {
            glDeleteProgram(h);
        }
    }
#else
    OGLShaderStage(OGLShaderStage&&) noexcept = default;
    OGLShaderStage& operator=(OGLShaderStage&&) noexcept = default;
    ~OGLShaderStage() = default;
#endif

    void Create(const char* source, GLenum type) {
        if (shader_or_program.index() == 0) {
            std::get<OGLShader>(shader_or_program).Create(source, type);
        } else {
            OGLShader shader;
            shader.Create(source, type);
            OGLProgram& program = std::get<OGLProgram>(shader_or_program);
            program.Create(true, std::array{shader.handle});
        }
    }

#ifdef __SWITCH__
    // Hands the source off to the worker. GetHandle() returns 0 until the
    // worker completes; callers must treat 0 as "skip this draw". `this`
    // must remain stable (unordered_map values are stable across rehashing)
    // and the AsyncShaderCompiler is destroyed before any OGLShaderStage,
    // so the captured `this` is always valid at callback time.
    void CreateAsync(u64 key, const char* source, GLenum type,
                     AsyncShaderCompiler& compiler) {
        compiler.Enqueue(key, type, std::string{source},
                         [this](GLuint program) {
                             async_handle.store(program, std::memory_order_release);
                         });
    }
#endif

    GLuint GetHandle() const {
#ifdef __SWITCH__
        const GLuint async = async_handle.load(std::memory_order_acquire);
        if (async != 0) {
            return async;
        }
#endif
        if (shader_or_program.index() == 0) {
            return std::get<OGLShader>(shader_or_program).handle;
        } else {
            return std::get<OGLProgram>(shader_or_program).handle;
        }
    }

    void Inject(OGLProgram&& program) {
        shader_or_program = std::move(program);
    }

private:
    std::variant<OGLShader, OGLProgram> shader_or_program;
#ifdef __SWITCH__
    std::atomic<GLuint> async_handle{0};
#endif
};

class TrivialVertexShader {
public:
    explicit TrivialVertexShader(const Driver& driver, bool separable) : program(separable) {
        const auto code =
            GLSL::GenerateTrivialVertexShader(driver.HasClipCullDistance(), separable);
        program.Create(code.c_str(), GL_VERTEX_SHADER);
    }
    GLuint Get() const {
        return program.GetHandle();
    }

private:
    OGLShaderStage program;
};

template <typename KeyConfigType, auto CodeGenerator, GLenum ShaderType>
class ShaderCache {
public:
    explicit ShaderCache(bool separable_) : separable{separable_} {}
    ~ShaderCache() = default;

#ifdef __SWITCH__
    void SetAsyncCompiler(AsyncShaderCompiler* compiler) {
        async_compiler = compiler;
    }
    // Used by LoadDiskCache to force the synchronous compile path during
    // warm-up; returns the previous value so the caller can restore it.
    AsyncShaderCompiler* SwapAsyncCompiler(AsyncShaderCompiler* next) {
        AsyncShaderCompiler* prev = async_compiler;
        async_compiler = next;
        return prev;
    }
#endif

    template <typename... Args>
    std::tuple<u64, GLuint, std::optional<std::string>> Get(const KeyConfigType& config,
                                                            Args&&... args) {
#ifdef __SWITCH__
        const auto switch_total_start = std::chrono::steady_clock::now();
        long long switch_codegen_ms = 0;
        long long switch_create_ms = 0;
        u64 switch_code_hash = 0;
#endif
        const size_t config_hash = config.Hash();
        auto map_it = shader_map.find(config_hash);
        if (map_it != shader_map.end()) {
            if (map_it->second == nullptr) {
                return {0, 0, std::nullopt};
            }
#ifdef __SWITCH__
            const auto switch_total_ms = SwitchGlShaderElapsedMs(switch_total_start);
            if (switch_total_ms >= 5 && SwitchGlShaderReserveLog()) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=opengl.shader.stagecache total-ms=%lld "
                    "codegen-ms=0 create-ms=0 key-hit=1 code-hit=1 created=0 type=%u "
                    "key=%016llX code=0 handle=%u",
                    switch_total_ms, static_cast<u32>(ShaderType),
                    static_cast<unsigned long long>(config_hash), map_it->second->GetHandle());
            }
#endif
            return {config_hash, map_it->second->GetHandle(), std::nullopt};
        }

#ifdef __SWITCH__
        const auto switch_codegen_start = std::chrono::steady_clock::now();
#endif
        auto generated = Common::HashableString(CodeGenerator(config, std::forward<Args>(args)...));
#ifdef __SWITCH__
        switch_codegen_ms = SwitchGlShaderElapsedMs(switch_codegen_start);
        switch_code_hash = generated.Hash();
#endif
        if (generated.empty()) {
            shader_map[config_hash] = nullptr;
            return {0, 0, std::nullopt};
        }

        auto [iter, new_shader] = shader_cache.emplace(generated.Hash(), OGLShaderStage{separable});
        OGLShaderStage& cached_shader = iter->second;
        std::optional<std::string> result{};
        result = std::move(generated);
        if (new_shader) {
#ifdef __SWITCH__
            const auto switch_create_start = std::chrono::steady_clock::now();
            if (async_compiler != nullptr) {
                // `generated` was moved into `result` above, so its internal
                // string is empty and generated.Hash() would collide for every
                // call. Use iter->first which is the pre-move hash captured at
                // emplace time.
                cached_shader.CreateAsync(iter->first, result->c_str(), ShaderType,
                                          *async_compiler);
                if (SwitchGlShaderReserveLog()) {
                    Azahar::Switch::AppendLogFormat(
                        nullptr,
                        "android-flow stage=opengl.async.compiler.dispatch type=%u "
                        "key=%016llX bytes=%zu",
                        static_cast<u32>(ShaderType),
                        static_cast<unsigned long long>(iter->first), result->size());
                }
            } else {
                cached_shader.Create(result->c_str(), ShaderType);
                if (SwitchGlShaderReserveLog()) {
                    Azahar::Switch::AppendLogFormat(
                        nullptr,
                        "android-flow stage=opengl.async.compiler.fallback-sync type=%u "
                        "key=%016llX bytes=%zu cache_ptr=%p",
                        static_cast<u32>(ShaderType),
                        static_cast<unsigned long long>(iter->first), result->size(),
                        static_cast<const void*>(this));
                }
            }
            switch_create_ms = SwitchGlShaderElapsedMs(switch_create_start);
#else
            cached_shader.Create(result->c_str(), ShaderType);
#endif
        }
        shader_map[config_hash] = &cached_shader;
#ifdef __SWITCH__
        const auto switch_total_ms = SwitchGlShaderElapsedMs(switch_total_start);
        if (switch_total_ms >= 5 && SwitchGlShaderReserveLog()) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=opengl.shader.stagecache total-ms=%lld codegen-ms=%lld "
                "create-ms=%lld key-hit=0 code-hit=%u created=%u type=%u key=%016llX "
                "code=%016llX handle=%u",
                switch_total_ms, switch_codegen_ms, switch_create_ms, new_shader ? 0U : 1U,
                new_shader ? 1U : 0U, static_cast<u32>(ShaderType),
                static_cast<unsigned long long>(config_hash),
                static_cast<unsigned long long>(switch_code_hash), cached_shader.GetHandle());
        }
#endif
        return {config_hash, cached_shader.GetHandle(), std::move(result)};
    }

    void Inject(const KeyConfigType& key, OGLProgram&& program) {
        OGLShaderStage stage{separable};
        stage.Inject(std::move(program));
        const auto iter = shader_cache.emplace(key.Hash(), std::move(stage)).first;
        OGLShaderStage& cached_shader = iter->second;
        shader_map.insert_or_assign(key.Hash(), &cached_shader);
    }

    void Inject(const KeyConfigType& key, std::string decomp, OGLProgram&& program) {
        OGLShaderStage stage{separable};
        stage.Inject(std::move(program));
        auto decomp_hash = Common::HashableString(std::move(decomp));
        const auto iter = shader_cache.emplace(decomp_hash.Hash(), std::move(stage)).first;
        OGLShaderStage& cached_shader = iter->second;
        shader_map.insert_or_assign(key.Hash(), &cached_shader);
    }

    void Inject(const KeyConfigType& key, OGLShaderStage&& stage) {
        const auto iter = shader_cache.emplace(key.Hash(), std::move(stage)).first;
        OGLShaderStage& cached_shader = iter->second;
        shader_map.insert_or_assign(key.Hash(), &cached_shader);
    }

    void Inject(const KeyConfigType& key, std::string decomp, OGLShaderStage&& stage) {
        auto decomp_hash = Common::HashableString(std::move(decomp));
        const auto iter = shader_cache.emplace(decomp_hash.Hash(), std::move(stage)).first;
        OGLShaderStage& cached_shader = iter->second;
        shader_map.insert_or_assign(key.Hash(), &cached_shader);
    }

private:
    bool separable;
    std::unordered_map<u64, OGLShaderStage*> shader_map;
    std::unordered_map<u64, OGLShaderStage> shader_cache;
#ifdef __SWITCH__
    AsyncShaderCompiler* async_compiler = nullptr;
#endif
};

// This is a cache designed for shaders translated from PICA shaders. The first cache matches the
// config structure like a normal cache does. On cache miss, the second cache matches the generated
// GLSL code. The configuration is like this because there might be leftover code in the PICA shader
// program buffer from the previous shader, which is hashed into the config, resulting several
// different config values from the same shader program.
template <typename KeyConfigType, typename ExtraConfigType,
          std::string (*CodeGenerator)(const Pica::ShaderSetup&, const KeyConfigType&,
                                       const ExtraConfigType&),
          GLenum ShaderType>
class ShaderDoubleCache {
public:
    explicit ShaderDoubleCache(bool _separable) : separable{_separable} {}
    std::tuple<u64, GLuint, std::optional<std::string>> Get(const KeyConfigType& key,
                                                            const ExtraConfigType& extra,
                                                            const Pica::ShaderSetup& setup) {
#ifdef __SWITCH__
        const auto switch_total_start = std::chrono::steady_clock::now();
        long long switch_codegen_ms = 0;
        long long switch_create_ms = 0;
        bool switch_generated_code = false;
        bool switch_created_shader = false;
        u64 switch_program_hash = 0;
#endif
        std::optional<std::string> result{};
        const size_t key_hash = key.Hash();
        auto map_it = shader_map.find(key_hash);
        if (map_it == shader_map.end()) {
#ifdef __SWITCH__
            const auto switch_codegen_start = std::chrono::steady_clock::now();
#endif
            auto program = Common::HashableString(CodeGenerator(setup, key, extra));
#ifdef __SWITCH__
            switch_codegen_ms = SwitchGlShaderElapsedMs(switch_codegen_start);
            switch_generated_code = true;
            switch_program_hash = program.Hash();
#endif
            if (program.empty()) {
                shader_map[key_hash] = nullptr;
                return {0, 0, std::nullopt};
            }

            auto [iter, new_shader] =
                shader_cache.emplace(program.Hash(), OGLShaderStage{separable});
            OGLShaderStage& cached_shader = iter->second;
            result = std::move(program);
            if (new_shader) {
#ifdef __SWITCH__
                const auto switch_create_start = std::chrono::steady_clock::now();
#endif
                cached_shader.Create((*result).c_str(), ShaderType);
#ifdef __SWITCH__
                switch_create_ms = SwitchGlShaderElapsedMs(switch_create_start);
                switch_created_shader = true;
#endif
            }
            shader_map[key_hash] = &cached_shader;
#ifdef __SWITCH__
            const auto switch_total_ms = SwitchGlShaderElapsedMs(switch_total_start);
            if (switch_total_ms >= 5 && SwitchGlShaderReserveLog()) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=opengl.shader.doublecache total-ms=%lld codegen-ms=%lld "
                    "create-ms=%lld key-hit=0 code-hit=%u created=%u type=%u key=%016llX "
                    "code=%016llX handle=%u",
                    switch_total_ms, switch_codegen_ms, switch_create_ms,
                    switch_created_shader ? 0U : 1U, switch_created_shader ? 1U : 0U,
                    static_cast<u32>(ShaderType), static_cast<unsigned long long>(key_hash),
                    static_cast<unsigned long long>(switch_program_hash), cached_shader.GetHandle());
            }
#endif
            return {key_hash, cached_shader.GetHandle(), std::move(result)};
        }

        if (map_it->second == nullptr) {
            return {0, 0, std::nullopt};
        }

#ifdef __SWITCH__
        const auto switch_total_ms = SwitchGlShaderElapsedMs(switch_total_start);
        if (switch_total_ms >= 5 && SwitchGlShaderReserveLog()) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=opengl.shader.doublecache total-ms=%lld codegen-ms=0 "
                "create-ms=0 key-hit=1 code-hit=1 created=0 type=%u key=%016llX code=0 "
                "handle=%u",
                switch_total_ms, static_cast<u32>(ShaderType),
                static_cast<unsigned long long>(key_hash), map_it->second->GetHandle());
        }
#endif
        return {key_hash, map_it->second->GetHandle(), std::nullopt};
    }

    void Inject(const KeyConfigType& key, std::string decomp, OGLProgram&& program) {
        OGLShaderStage stage{separable};
        stage.Inject(std::move(program));
        auto decomp_hash = Common::HashableString(std::move(decomp));
        const auto iter = shader_cache.emplace(decomp_hash.Hash(), std::move(stage)).first;
        OGLShaderStage& cached_shader = iter->second;
        shader_map.insert_or_assign(key.Hash(), &cached_shader);
    }

    void Inject(const KeyConfigType& key, std::string decomp, OGLShaderStage&& stage) {
        auto decomp_hash = Common::HashableString(std::move(decomp));
        const auto iter = shader_cache.emplace(decomp_hash.Hash(), std::move(stage)).first;
        OGLShaderStage& cached_shader = iter->second;
        shader_map.insert_or_assign(key.Hash(), &cached_shader);
    }

private:
    bool separable;
    std::unordered_map<u64, OGLShaderStage*> shader_map;
    std::unordered_map<u64, OGLShaderStage> shader_cache;
};

using ProgrammableVertexShaders =
    ShaderDoubleCache<PicaVSConfig, ExtraVSConfig, &GLSL::GenerateVertexShader, GL_VERTEX_SHADER>;

using FixedGeometryShaders =
    ShaderCache<PicaFixedGSConfig, &GLSL::GenerateFixedGeometryShader, GL_GEOMETRY_SHADER>;

using FragmentShaders = ShaderCache<FSConfig, &GLSL::GenerateFragmentShader, GL_FRAGMENT_SHADER>;

class ShaderProgramManager::Impl {
public:
    explicit Impl(const Driver& driver, u64 title_id, bool separable)
        : separable(separable), programmable_vertex_shaders(separable),
          trivial_vertex_shader(driver, separable), fixed_geometry_shaders(separable),
          fragment_shaders(separable), disk_cache(title_id, separable) {
        if (separable) {
            pipeline.Create();
        }
        profile = Pica::Shader::Profile{
            .has_separable_shaders = separable,
            .has_clip_planes = driver.HasClipCullDistance(),
            .has_geometry_shader = true,
            .has_custom_border_color = true,
            .has_fragment_shader_interlock = driver.HasArbFragmentShaderInterlock(),
            // TODO: This extension requires GLSL 450 / OpenGL 4.5 context.
            .has_fragment_shader_barycentric = false,
            .has_blend_minmax_factor = driver.HasBlendMinMaxFactor(),
            .has_minus_one_to_one_range = true,
            .has_logic_op = !driver.IsOpenGLES(),
            .has_gl_ext_framebuffer_fetch = driver.HasExtFramebufferFetch(),
            .has_gl_arm_framebuffer_fetch = driver.HasArmShaderFramebufferFetch(),
            .has_gl_nv_fragment_shader_interlock = driver.HasNvFragmentShaderInterlock(),
            .has_gl_intel_fragment_shader_ordering = driver.HasIntelFragmentShaderOrdering(),
            // TODO: This extension requires GLSL 450 / OpenGL 4.5 context.
            .has_gl_nv_fragment_shader_barycentric = false,
            .is_vulkan = false,
        };
#ifdef __SWITCH__
        if (separable && Settings::values.async_shader_compilation.GetValue()) {
            async_compiler = AsyncShaderCompiler::Create();
            if (async_compiler) {
                fragment_shaders.SetAsyncCompiler(async_compiler.get());
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=opengl.async.compiler.wired target=fragment-shaders "
                    "title=%016llX cache_ptr=%p compiler_ptr=%p",
                    static_cast<unsigned long long>(title_id),
                    static_cast<const void*>(&fragment_shaders),
                    static_cast<const void*>(async_compiler.get()));
            }
        }
#endif
    }

    struct ShaderTuple {
        std::size_t vs_hash = 0;
        std::size_t gs_hash = 0;
        std::size_t fs_hash = 0;

        GLuint vs = 0;
        GLuint gs = 0;
        GLuint fs = 0;

        bool operator==(const ShaderTuple& rhs) const {
            return std::tie(vs, gs, fs) == std::tie(rhs.vs, rhs.gs, rhs.fs);
        }

        bool operator!=(const ShaderTuple& rhs) const {
            return std::tie(vs, gs, fs) != std::tie(rhs.vs, rhs.gs, rhs.fs);
        }

        std::size_t GetConfigHash() const {
            return Common::ComputeHash64(this, sizeof(std::size_t) * 3);
        }
    };

    static_assert(offsetof(ShaderTuple, vs_hash) == 0, "ShaderTuple layout changed!");
    static_assert(offsetof(ShaderTuple, fs_hash) == sizeof(std::size_t) * 2,
                  "ShaderTuple layout changed!");

    bool separable;
    Pica::Shader::Profile profile{};
    ShaderTuple current;

    ProgrammableVertexShaders programmable_vertex_shaders;
    TrivialVertexShader trivial_vertex_shader;

    FixedGeometryShaders fixed_geometry_shaders;

    FragmentShaders fragment_shaders;
    std::unordered_map<u64, OGLProgram> program_cache;
    OGLPipeline pipeline;
    ShaderDiskCache disk_cache;
#ifdef __SWITCH__
    // Declared LAST so it is destroyed FIRST during Impl teardown. The
    // compiler's destructor joins the worker thread, guaranteeing no callback
    // can race against OGLShaderStage destruction in the caches above.
    std::unique_ptr<AsyncShaderCompiler> async_compiler;
#endif

    Pica::Shader::Generator::ExtraVSConfig CalcExtraConfig(
        const Pica::Shader::Generator::PicaVSConfig& config, bool accurate_mul) {
        auto res = ExtraVSConfig();

        // Enable the geometry-shader only if we are actually doing per-fragment lighting
        // and care about proper quaternions. Otherwise just use standard vertex+fragment shaders.
        const bool use_geometry_shader = !config.state.lighting_disable;

        res.use_clip_planes = profile.has_clip_planes;
        res.use_geometry_shader = use_geometry_shader;
        res.sanitize_mul = accurate_mul;
        res.separable_shader = separable;
        res.load_flags.fill(AttribLoadFlags::Float);

        return res;
    }
};

ShaderProgramManager::ShaderProgramManager(Frontend::EmuWindow& emu_window_, const Driver& driver_,
                                           u64 title_id, bool separable)
    : emu_window{emu_window_}, driver{driver_},
      strict_context_required{emu_window.StrictContextRequired()},
      impl{std::make_unique<Impl>(driver_, title_id, separable)} {}

ShaderProgramManager::~ShaderProgramManager() = default;

bool ShaderProgramManager::UseProgrammableVertexShader(const Pica::RegsInternal& regs,
                                                       Pica::ShaderSetup& setup,
                                                       bool accurate_mul) {
#ifdef __SWITCH__
    const auto switch_start = std::chrono::steady_clock::now();
#endif

    PicaVSConfig config{regs, setup};
    ExtraVSConfig extra = impl->CalcExtraConfig(config, accurate_mul);

    auto [hash, handle, result] = impl->programmable_vertex_shaders.Get(config, extra, setup);
    if (handle == 0) {
#ifdef __SWITCH__
        const auto switch_ms = SwitchGlShaderElapsedMs(switch_start);
        if (SwitchGlShaderReserveLog()) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=opengl.vs.use result=0 elapsed-ms=%lld new-shader=0 "
                "handle=0 hash=%016llX",
                switch_ms, static_cast<unsigned long long>(hash));
        }
#endif
        return false;
    }
    impl->current.vs = handle;
    impl->current.vs_hash = hash;

    // Save VS to the disk cache if its a new shader
    if (result) {
        auto& disk_cache = impl->disk_cache;
#ifdef __SWITCH__
        const auto switch_cache_save_start = std::chrono::steady_clock::now();
#endif
        const auto& program_code = setup.GetProgramCode();
        const auto& swizzle_data = setup.GetSwizzleData();
        ProgramCode new_program_code{program_code.begin(), program_code.end()};
        new_program_code.insert(new_program_code.end(), swizzle_data.begin(), swizzle_data.end());
        const u64 unique_identifier = GetUniqueIdentifier(regs, new_program_code);
        const ShaderDiskCacheRaw raw{unique_identifier, ProgramType::VS, regs,
                                     std::move(new_program_code)};
        disk_cache.SaveRaw(raw);
        disk_cache.SaveDecompiled(unique_identifier, *result, accurate_mul);
#ifdef __SWITCH__
        if (impl->separable) {
            disk_cache.SaveDump(unique_identifier, handle);
            disk_cache.SaveVirtualPrecompiledFile();
        }
        const auto switch_cache_save_ms = SwitchGlShaderElapsedMs(switch_cache_save_start);
        if (SwitchGlShaderReserveLog()) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=opengl.shader.cache.save elapsed-ms=%lld separable=%u "
                "program=%016llX shader=%016llX handle=%u code-bytes=%zu",
                switch_cache_save_ms, impl->separable ? 1U : 0U,
                static_cast<unsigned long long>(disk_cache.GetProgramID()),
                static_cast<unsigned long long>(unique_identifier), handle, result->size());
        }
#endif
    }
#ifdef __SWITCH__
    const auto switch_ms = SwitchGlShaderElapsedMs(switch_start);
    if (switch_ms >= 5 && SwitchGlShaderReserveLog()) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=opengl.vs.use result=1 elapsed-ms=%lld new-shader=%u "
            "handle=%u hash=%016llX",
            switch_ms, result ? 1U : 0U, handle, static_cast<unsigned long long>(hash));
    }
#endif
    return true;
}

void ShaderProgramManager::UseTrivialVertexShader() {
    impl->current.vs = impl->trivial_vertex_shader.Get();
    impl->current.vs_hash = 0;
}

void ShaderProgramManager::UseFixedGeometryShader(const Pica::RegsInternal& regs) {
    PicaFixedGSConfig gs_config(regs);
    ExtraFixedGSConfig extra{
        .use_clip_planes = driver.HasClipCullDistance(),
        .separable_shader = impl->separable,
    };

    auto [hash, handle, _] = impl->fixed_geometry_shaders.Get(gs_config, extra);
    impl->current.gs = handle;
    impl->current.gs_hash = hash;
}

void ShaderProgramManager::UseTrivialGeometryShader() {
    impl->current.gs = 0;
    impl->current.gs_hash = 0;
}

void ShaderProgramManager::UseFragmentShader(const Pica::RegsInternal& regs,
                                             const Pica::Shader::UserConfig& user) {
    const FSConfig fs_config{regs};
    auto [hash, handle, result] = impl->fragment_shaders.Get(fs_config, user, impl->profile);
    impl->current.fs = handle;
    impl->current.fs_hash = hash;
    // Save FS to the disk cache if its a new shader
    if (result) {
        auto& disk_cache = impl->disk_cache;
        u64 unique_identifier = GetUniqueIdentifier(regs, {});
        ShaderDiskCacheRaw raw{unique_identifier, ProgramType::FS, regs, {}};
        disk_cache.SaveRaw(raw);
        disk_cache.SaveDecompiled(unique_identifier, *result, false);
#ifdef __SWITCH__
        // Mirror the VS path: in separable mode each shader is its own GL
        // program, so we have to persist the compiled FS binary ourselves —
        // otherwise the precompiled cache stays empty and every cold launch
        // re-links every fragment shader from GLSL source. Fragment programs
        // are the ones taking ~1s per link on Tegra/Mesa.
        // handle is 0 when async compilation is still in flight — skip the
        // binary save in that case; the dump call would be invalid on a zero
        // program. A future iteration could trigger save from the async
        // callback, but binary cache load is broken on Switch (see
        // gl_shader_manager.cpp::LoadPrecompiledShader fallback) so this is
        // pure best-effort for now.
        if (impl->separable && handle != 0) {
            const auto switch_save_start = std::chrono::steady_clock::now();
            disk_cache.SaveDump(unique_identifier, handle);
            disk_cache.SaveVirtualPrecompiledFile();
            const auto switch_save_ms = SwitchGlShaderElapsedMs(switch_save_start);
            if (SwitchGlShaderReserveLog()) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=opengl.shader.cache.save.fs elapsed-ms=%lld "
                    "program=%016llX shader=%016llX handle=%u code-bytes=%zu",
                    switch_save_ms,
                    static_cast<unsigned long long>(disk_cache.GetProgramID()),
                    static_cast<unsigned long long>(unique_identifier), handle,
                    result->size());
            }
        }
#endif
    }
}

bool ShaderProgramManager::ApplyTo(OpenGLState& state, bool accurate_mul) {
    if (impl->separable) {
#ifdef __SWITCH__
        // Re-fetch handles via GetHandle (which checks async state) since
        // the cached impl->current.fs may have been 0 last frame; we want a
        // fresh atomic load here. impl->current.fs gets set in
        // UseFragmentShader, which is called every draw, so it already
        // reflects the latest atomic. Just skip if still not ready.
        if (impl->current.fs == 0) {
            return false;
        }
#endif
        if (driver.HasBug(DriverBug::ShaderStageChangeFreeze)) {
            glUseProgramStages(
                impl->pipeline.handle,
                GL_VERTEX_SHADER_BIT | GL_GEOMETRY_SHADER_BIT | GL_FRAGMENT_SHADER_BIT, 0);
        }

        glUseProgramStages(impl->pipeline.handle, GL_VERTEX_SHADER_BIT, impl->current.vs);
        glUseProgramStages(impl->pipeline.handle, GL_GEOMETRY_SHADER_BIT, impl->current.gs);
        glUseProgramStages(impl->pipeline.handle, GL_FRAGMENT_SHADER_BIT, impl->current.fs);
        state.draw.shader_program = 0;
        state.draw.program_pipeline = impl->pipeline.handle;
    } else {
        const u64 unique_identifier = impl->current.GetConfigHash();
        OGLProgram& cached_program = impl->program_cache[unique_identifier];
        if (cached_program.handle == 0) {
            cached_program.Create(false,
                                  std::array{impl->current.vs, impl->current.gs, impl->current.fs});
            auto& disk_cache = impl->disk_cache;
            disk_cache.SaveDumpToFile(unique_identifier, cached_program.handle, accurate_mul);
        }
        state.draw.shader_program = cached_program.handle;
    }
    return true;
}

u64 ShaderProgramManager::GetProgramID() const {
    return impl->disk_cache.GetProgramID();
}

void ShaderProgramManager::LoadDiskCache(const std::atomic_bool& stop_loading,
                                         const VideoCore::DiskResourceLoadCallback& callback,
                                         bool accurate_mul) {
    auto& disk_cache = impl->disk_cache;
#ifdef __SWITCH__
    // The load path treats handle==0 from Get() as a hard failure (line ~1043,
    // "compilation from raw failed") and nukes the cache. With the async
    // compiler wired up, every FS in the cache load would return 0 and the
    // whole load would abort. The load is itself an offline warm-up so force
    // sync compile here; restore the async pointer when load is done.
    AsyncShaderCompiler* saved_compiler = impl->fragment_shaders.SwapAsyncCompiler(nullptr);
    struct AsyncRestore {
        FragmentShaders* cache;
        AsyncShaderCompiler* prev;
        ~AsyncRestore() {
            cache->SwapAsyncCompiler(prev);
        }
    } restore_async{&impl->fragment_shaders, saved_compiler};
#endif
#ifdef __SWITCH__
    if (SwitchGlShaderReserveLog()) {
        const auto shader_dir = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir);
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=opengl.shader.cache.load.begin program=%016llX separable=%u "
            "disk=%u hw=%u accurate-mul=%u shader-dir=\"%s\"",
            static_cast<unsigned long long>(disk_cache.GetProgramID()),
            impl->separable ? 1U : 0U,
            Settings::values.use_disk_shader_cache.GetValue() ? 1U : 0U,
            Settings::values.use_hw_shader.GetValue() ? 1U : 0U, accurate_mul ? 1U : 0U,
            shader_dir.c_str());
    }
#endif
    const auto transferable = disk_cache.LoadTransferable();
    if (!transferable) {
#ifdef __SWITCH__
        if (SwitchGlShaderReserveLog()) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=opengl.shader.cache.load.skip program=%016llX separable=%u",
                static_cast<unsigned long long>(disk_cache.GetProgramID()),
                impl->separable ? 1U : 0U);
        }
#endif
        return;
    }
    const auto& raws = *transferable;

    // Load uncompressed precompiled file for non-separable shaders.
    // Precompiled file for separable shaders is compressed.
    auto [decompiled, dumps] = disk_cache.LoadPrecompiled(impl->separable);
#ifdef __SWITCH__
    if (SwitchGlShaderReserveLog()) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=opengl.shader.cache.load.entries program=%016llX separable=%u "
            "raws=%zu decompiled=%zu dumps=%zu",
            static_cast<unsigned long long>(disk_cache.GetProgramID()), impl->separable ? 1U : 0U,
            raws.size(), decompiled.size(), dumps.size());
    }
#endif

    if (stop_loading) {
        return;
    }

    std::set<GLenum> supported_formats = GetSupportedFormats();

    // Track if precompiled cache was altered during loading to know if we have to serialize the
    // virtual precompiled cache file back to the hard drive
    bool precompiled_cache_altered = false;

    std::mutex mutex;
    std::atomic_bool compilation_failed = false;
    if (callback) {
        callback(VideoCore::LoadCallbackStage::Decompile, 0, raws.size(), "");
    }
    std::vector<std::size_t> load_raws_index;
    // Loads both decompiled and precompiled shaders from the cache. If either one is missing for
    const auto LoadPrecompiledShader = [&](std::size_t begin, std::size_t end,
                                           std::span<const ShaderDiskCacheRaw> raw_cache,
                                           const ShaderDecompiledMap& decompiled_map,
                                           const ShaderDumpsMap& dump_map) {
        for (std::size_t i = begin; i < end; ++i) {
            if (stop_loading || compilation_failed) {
                return;
            }
            const auto& raw{raw_cache[i]};
            const u64 unique_identifier{raw.GetUniqueIdentifier()};

            const u64 calculated_hash =
                GetUniqueIdentifier(raw.GetRawShaderConfig(), raw.GetProgramCode());
            if (unique_identifier != calculated_hash) {
                LOG_ERROR(Render_OpenGL,
                          "Invalid hash in entry={:016x} (obtained hash={:016x}) - removing "
                          "shader cache",
                          raw.GetUniqueIdentifier(), calculated_hash);
                disk_cache.InvalidateAll();
                return;
            }

            const auto dump{dump_map.find(unique_identifier)};
            const auto decomp{decompiled_map.find(unique_identifier)};

            OGLProgram shader;

            if (dump != dump_map.end() && decomp != decompiled_map.end()) {
                // Only load the vertex shader if its sanitize_mul setting matches
                if (raw.GetProgramType() == ProgramType::VS &&
                    decomp->second.sanitize_mul != accurate_mul) {
                    std::scoped_lock lock(mutex);
                    load_raws_index.push_back(i);
                    continue;
                }

                // If the shader is dumped, attempt to load it
                shader =
                    GeneratePrecompiledProgram(dump->second, supported_formats, impl->separable);
                if (shader.handle == 0) {
#ifdef __SWITCH__
                    std::scoped_lock lock(mutex);
                    load_raws_index.push_back(i);
                    precompiled_cache_altered = true;
                    continue;
#else
                    // If any shader failed, stop trying to compile, delete the cache, and start
                    // loading from raws
                    compilation_failed = true;
                    return;
#endif
                }
                // we have both the binary shader and the decompiled, so inject it into the
                // cache
                if (raw.GetProgramType() == ProgramType::VS) {
                    auto [conf, setup] = BuildVSConfigFromRaw(raw, driver, accurate_mul);
                    std::scoped_lock lock(mutex);
                    impl->programmable_vertex_shaders.Inject(conf, decomp->second.code,
                                                             std::move(shader));
                } else if (raw.GetProgramType() == ProgramType::FS) {
                    // TODO: Support UserConfig in disk shader cache
                    const FSConfig conf(raw.GetRawShaderConfig());
                    std::scoped_lock lock(mutex);
                    impl->fragment_shaders.Inject(conf, decomp->second.code, std::move(shader));
                } else {
                    // Unsupported shader type got stored somehow so nuke the cache

                    LOG_CRITICAL(Frontend, "failed to load raw ProgramType {}",
                                 raw.GetProgramType());
                    compilation_failed = true;
                    return;
                }
            } else {
                // Since precompiled didn't have the dump, we'll load them in the next phase
                std::scoped_lock lock(mutex);
                load_raws_index.push_back(i);
            }
            if (callback) {
                callback(VideoCore::LoadCallbackStage::Decompile, i, raw_cache.size(), "");
            }
        }
    };

    const auto LoadPrecompiledProgram = [&](const ShaderDecompiledMap& decompiled_map,
                                            const ShaderDumpsMap& dump_map) {
        std::size_t i{0};
        for (const auto& dump : dump_map) {
            if (stop_loading) {
                break;
            }
            const u64 unique_identifier{dump.first};
            const auto decomp{decompiled_map.find(unique_identifier)};

            // Only load the program if its sanitize_mul setting matches
            if (decomp->second.sanitize_mul != accurate_mul) {
                continue;
            }

            // If the shader program is dumped, attempt to load it
            OGLProgram shader =
                GeneratePrecompiledProgram(dump.second, supported_formats, impl->separable);
            if (shader.handle != 0) {
                impl->program_cache.emplace(unique_identifier, std::move(shader));
            } else {
                LOG_ERROR(Frontend, "Failed to link Precompiled program!");
                compilation_failed = true;
                break;
            }
            if (callback) {
                callback(VideoCore::LoadCallbackStage::Decompile, ++i, dump_map.size(), "");
            }
        }
    };

    if (impl->separable) {
        LoadPrecompiledShader(0, raws.size(), raws, decompiled, dumps);
    } else {
        LoadPrecompiledProgram(decompiled, dumps);
    }

    bool load_all_raws = false;
    if (compilation_failed) {
        // Invalidate the precompiled cache if a shader dumped shader was rejected
        impl->program_cache.clear();
        disk_cache.InvalidatePrecompiled();
        dumps.clear();
        precompiled_cache_altered = true;
        load_all_raws = true;
    }
    // TODO(SachinV): Skip loading raws until we implement a proper way to link non-seperable
    // shaders.
    if (!impl->separable) {
        return;
    }

    const std::size_t load_raws_size = load_all_raws ? raws.size() : load_raws_index.size();

    if (callback) {
        callback(VideoCore::LoadCallbackStage::Build, 0, load_raws_size, "Shader");
    }

    compilation_failed = false;

    std::size_t built_shaders = 0; // It doesn't have be atomic since it's used behind a mutex
    const auto LoadRawSepareble = [&](std::size_t begin, std::size_t end,
                                      Frontend::GraphicsContext* context = nullptr) {
        const auto scope = context->Acquire();
        for (std::size_t i = begin; i < end; ++i) {
            if (stop_loading || compilation_failed) {
                return;
            }

            const std::size_t raws_index = load_all_raws ? i : load_raws_index[i];
            const auto& raw{raws[raws_index]};
            const u64 unique_identifier{raw.GetUniqueIdentifier()};

            bool sanitize_mul = false;
            GLuint handle{0};
            std::string code;
            // Otherwise decompile and build the shader at boot and save the result to the
            // precompiled file
            if (raw.GetProgramType() == ProgramType::VS) {
                auto [conf, setup] = BuildVSConfigFromRaw(raw, driver, accurate_mul);
                ExtraVSConfig extra = impl->CalcExtraConfig(conf, accurate_mul);
                sanitize_mul = accurate_mul;
                std::scoped_lock lock(mutex);
                auto [_, shader_handle, result] =
                    impl->programmable_vertex_shaders.Get(conf, extra, setup);
                handle = shader_handle;
                if (result) {
                    code = std::move(*result);
                }
            } else if (raw.GetProgramType() == ProgramType::FS) {
                // TODO: Support UserConfig in disk shader cache
                const FSConfig fs_config{raw.GetRawShaderConfig()};
                std::scoped_lock lock(mutex);
                auto [_, shader_handle, result] =
                    impl->fragment_shaders.Get(fs_config, Pica::Shader::UserConfig{}, impl->profile);
                handle = shader_handle;
                if (result) {
                    code = std::move(*result);
                }
            } else {
                // Unsupported shader type got stored somehow so nuke the cache
                LOG_ERROR(Frontend, "failed to load raw ProgramType {}", raw.GetProgramType());
                compilation_failed = true;
                return;
            }
            if (handle == 0) {
                LOG_ERROR(Frontend, "compilation from raw failed {:x} {:x}",
                          raw.GetProgramCode().at(0), raw.GetProgramCode().at(1));
                compilation_failed = true;
                return;
            }

            std::scoped_lock lock(mutex);
            // If this is a new separable shader, add it the precompiled cache
            if (!code.empty()) {
                disk_cache.SaveDecompiled(unique_identifier, code, sanitize_mul);
                disk_cache.SaveDump(unique_identifier, handle);
                precompiled_cache_altered = true;
            }

            if (callback) {
                callback(VideoCore::LoadCallbackStage::Build, ++built_shaders, load_raws_size,
                         "Shader");
            }
        }
    };

    if (!strict_context_required) {
        const std::size_t num_workers{std::max(1U, std::thread::hardware_concurrency())};
        const std::size_t bucket_size{load_raws_size / num_workers};
        std::vector<std::unique_ptr<Frontend::GraphicsContext>> contexts(num_workers);
        std::vector<std::thread> threads(num_workers);

        emu_window.SaveContext();
        for (std::size_t i = 0; i < num_workers; ++i) {
            const bool is_last_worker = i + 1 == num_workers;
            const std::size_t start{bucket_size * i};
            const std::size_t end{is_last_worker ? load_raws_size : start + bucket_size};

            // On some platforms the shared context has to be created from the GUI thread
            contexts[i] = emu_window.CreateSharedContext();
            // Release the context, so it can be immediately used by the spawned thread
            contexts[i]->DoneCurrent();
            threads[i] = std::thread(LoadRawSepareble, start, end, contexts[i].get());
        }
        for (auto& thread : threads) {
            thread.join();
        }
        emu_window.RestoreContext();
    } else {
        const auto dummy_context{std::make_unique<Frontend::GraphicsContext>()};
        LoadRawSepareble(0, load_raws_size, dummy_context.get());
    }

    if (compilation_failed) {
        disk_cache.InvalidateAll();
    }

    if (precompiled_cache_altered) {
        disk_cache.SaveVirtualPrecompiledFile();
    }
}

} // namespace OpenGL
