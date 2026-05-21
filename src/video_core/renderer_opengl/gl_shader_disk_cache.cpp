// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <fmt/format.h>

#include "common/common_paths.h"
#include "common/common_types.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/zstd_compression.h"
#include "video_core/renderer_opengl/gl_shader_disk_cache.h"

#ifdef __SWITCH__
namespace Azahar::Switch {
bool AppendLogFormat(int* error_out, const char* format, ...);
}
#endif

namespace OpenGL {

#ifdef __SWITCH__
namespace {
void SwitchShaderDiskCacheLogFile(const char* stage, u64 program_id, bool separable,
                                  const std::string& path, u64 size, u64 pos) {
    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=opengl.shader.cache.%s program=%016llX separable=%u "
        "size=%llu pos=%llu path=\"%s\"",
        stage, static_cast<unsigned long long>(program_id), separable ? 1U : 0U,
        static_cast<unsigned long long>(size), static_cast<unsigned long long>(pos),
        path.c_str());
}
}
#endif

constexpr std::size_t HASH_LENGTH = 64;
using ShaderCacheVersionHash = std::array<u8, HASH_LENGTH>;

enum class TransferableEntryKind : u32 {
    Raw,
};

enum class PrecompiledEntryKind : u32 {
    Decompiled,
    Dump,
};

constexpr u32 NativeVersion = 1;

// The hash is based on relevant files. The list of files can be found at src/common/CMakeLists.txt
// and CMakeModules/GenerateSCMRev.cmake
ShaderCacheVersionHash GetShaderCacheVersionHash() {
    ShaderCacheVersionHash hash{};
    const std::size_t length = std::min(std::strlen(Common::g_shader_cache_version), hash.size());
    std::memcpy(hash.data(), Common::g_shader_cache_version, length);
    return hash;
}

ShaderDiskCacheRaw::ShaderDiskCacheRaw(u64 unique_identifier, ProgramType program_type,
                                       RawShaderConfig config, ProgramCode program_code)
    : unique_identifier{unique_identifier}, program_type{program_type}, config{config},
      program_code{std::move(program_code)} {}

bool ShaderDiskCacheRaw::Load(FileUtil::IOFile& file) {
    if (file.ReadBytes(&unique_identifier, sizeof(u64)) != sizeof(u64) ||
        file.ReadBytes(&program_type, sizeof(u32)) != sizeof(u32)) {
        return false;
    }

    u64 reg_array_len{};
    if (file.ReadBytes(&reg_array_len, sizeof(u64)) != sizeof(u64)) {
        return false;
    }

    if (file.ReadArray(config.reg_array.data(), reg_array_len) != reg_array_len) {
        return false;
    }

    // Read in type specific configuration
    if (program_type == ProgramType::VS) {
        u64 code_len{};
        if (file.ReadBytes(&code_len, sizeof(u64)) != sizeof(u64)) {
            return false;
        }
        program_code.resize(code_len);
        if (file.ReadArray(program_code.data(), code_len) != code_len) {
            return false;
        }
    }

    return true;
}

bool ShaderDiskCacheRaw::Save(FileUtil::IOFile& file) const {
    if (file.WriteObject(unique_identifier) != 1 ||
        file.WriteObject(static_cast<u32>(program_type)) != 1) {
        return false;
    }

    // Just for future proofing, save the sizes of the array to the file
    const std::size_t reg_array_len = Pica::RegsInternal::NUM_REGS;
    if (file.WriteObject(static_cast<u64>(reg_array_len)) != 1) {
        return false;
    }
    if (file.WriteArray(config.reg_array.data(), reg_array_len) != reg_array_len) {
        return false;
    }

    if (program_type == ProgramType::VS) {
        const std::size_t code_len = program_code.size();
        if (file.WriteObject(static_cast<u64>(code_len)) != 1) {
            return false;
        }
        if (file.WriteArray(program_code.data(), code_len) != code_len) {
            return false;
        }
    }
    return true;
}

ShaderDiskCache::ShaderDiskCache(u64 program_id, bool separable)
    : separable{separable}, program_id{program_id}, transferable_file(AppendTransferableFile()),
      // seperable shaders use the virtual precompile file, that already has a header.
      precompiled_file(AppendPrecompiledFile(!separable)) {}

std::optional<std::vector<ShaderDiskCacheRaw>> ShaderDiskCache::LoadTransferable() {
    const bool has_title_id = GetProgramID() != 0;
    if (!Settings::values.use_hw_shader || !Settings::values.use_disk_shader_cache ||
        !has_title_id) {
#ifdef __SWITCH__
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=opengl.shader.cache.transferable.disabled program=%016llX "
            "separable=%u hw=%u disk=%u has-title=%u",
            static_cast<unsigned long long>(GetProgramID()), separable ? 1U : 0U,
            Settings::values.use_hw_shader ? 1U : 0U,
            Settings::values.use_disk_shader_cache ? 1U : 0U, has_title_id ? 1U : 0U);
#endif
        return std::nullopt;
    }
    tried_to_load = true;

    const auto transferable_path{GetTransferablePath()};
    if (!transferable_file.IsOpen()) {
#ifdef __SWITCH__
        SwitchShaderDiskCacheLogFile("transferable.closed", GetProgramID(), separable,
                                     transferable_path, 0, 0);
#endif
        LOG_ERROR(Render_OpenGL, "Transferable shader cache is not open for title id={}",
                  GetTitleID());
        return std::nullopt;
    }

    const u64 transferable_size = transferable_file.GetSize();
#ifdef __SWITCH__
    SwitchShaderDiskCacheLogFile("transferable.load.begin", GetProgramID(), separable,
                                 transferable_path, transferable_size, transferable_file.Tell());
#endif

    if (transferable_size == 0) {
#ifdef __SWITCH__
        SwitchShaderDiskCacheLogFile("transferable.empty", GetProgramID(), separable,
                                     transferable_path, transferable_size,
                                     transferable_file.Tell());
#endif
        LOG_INFO(Render_OpenGL, "No transferable shader cache found for game with title id={}",
                 GetTitleID());
        return std::nullopt;
    }

    if (!transferable_file.Seek(0, SEEK_SET)) {
#ifdef __SWITCH__
        SwitchShaderDiskCacheLogFile("transferable.seek-fail", GetProgramID(), separable,
                                     transferable_path, transferable_size,
                                     transferable_file.Tell());
#endif
        LOG_ERROR(Render_OpenGL, "Failed to seek transferable cache for title id={}",
                  GetTitleID());
        return std::nullopt;
    }

    u32 version{};
    if (transferable_file.ReadBytes(&version, sizeof(version)) != sizeof(version)) {
#ifdef __SWITCH__
        SwitchShaderDiskCacheLogFile("transferable.version-read-fail", GetProgramID(), separable,
                                     transferable_path, transferable_size,
                                     transferable_file.Tell());
#endif
        LOG_ERROR(Render_OpenGL,
                  "Failed to get transferable cache version for title id={} - removing",
                  GetTitleID());
        InvalidateAll();
        return std::nullopt;
    }

    if (version < NativeVersion) {
#ifdef __SWITCH__
        SwitchShaderDiskCacheLogFile("transferable.old-version", GetProgramID(), separable,
                                     transferable_path, transferable_size,
                                     transferable_file.Tell());
#endif
        LOG_INFO(Render_OpenGL, "Transferable shader cache is old - removing");
        InvalidateAll();
        return std::nullopt;
    }
    if (version > NativeVersion) {
#ifdef __SWITCH__
        SwitchShaderDiskCacheLogFile("transferable.new-version", GetProgramID(), separable,
                                     transferable_path, transferable_size,
                                     transferable_file.Tell());
#endif
        LOG_WARNING(Render_OpenGL, "Transferable shader cache was generated with a newer version "
                                   "of the emulator - skipping");
        return std::nullopt;
    }

    // Version is valid, load the shaders
    std::vector<ShaderDiskCacheRaw> raws;
    while (transferable_file.Tell() < transferable_size) {
        TransferableEntryKind kind{};
        if (transferable_file.ReadBytes(&kind, sizeof(u32)) != sizeof(u32)) {
#ifdef __SWITCH__
            SwitchShaderDiskCacheLogFile("transferable.kind-read-fail", GetProgramID(), separable,
                                         transferable_path, transferable_size,
                                         transferable_file.Tell());
#endif
            LOG_ERROR(Render_OpenGL, "Failed to read transferable file - removing");
            InvalidateAll();
            return std::nullopt;
        }

        switch (kind) {
        case TransferableEntryKind::Raw: {
            ShaderDiskCacheRaw entry;
            if (!entry.Load(transferable_file)) {
#ifdef __SWITCH__
                SwitchShaderDiskCacheLogFile("transferable.raw-read-fail", GetProgramID(),
                                             separable, transferable_path, transferable_size,
                                             transferable_file.Tell());
#endif
                LOG_ERROR(Render_OpenGL, "Failed to load transferable raw entry - removing");
                InvalidateAll();
                return std::nullopt;
            }
            transferable.emplace(entry.GetUniqueIdentifier(), ShaderDiskCacheRaw{});
            raws.push_back(std::move(entry));
            break;
        }
        default:
#ifdef __SWITCH__
            SwitchShaderDiskCacheLogFile("transferable.unknown-kind", GetProgramID(), separable,
                                         transferable_path, transferable_size,
                                         transferable_file.Tell());
#endif
            LOG_ERROR(Render_OpenGL, "Unknown transferable shader cache entry kind={} - removing",
                      kind);
            InvalidateAll();
            return std::nullopt;
        }
    }

#ifdef __SWITCH__
    SwitchShaderDiskCacheLogFile("transferable.load.end", GetProgramID(), separable,
                                 transferable_path, transferable_size, transferable_file.Tell());
    Azahar::Switch::AppendLogFormat(
        nullptr,
        "android-flow stage=opengl.shader.cache.transferable.entries program=%016llX "
        "separable=%u entries=%llu",
        static_cast<unsigned long long>(GetProgramID()), separable ? 1U : 0U,
        static_cast<unsigned long long>(raws.size()));
#endif
    LOG_INFO(Render_OpenGL, "Found a transferable disk cache with {} entries", raws.size());
    return {std::move(raws)};
}

std::pair<std::unordered_map<u64, ShaderDiskCacheDecompiled>, ShaderDumpsMap>
ShaderDiskCache::LoadPrecompiled(bool compressed) {
    if (!IsUsable())
        return {};

    if (precompiled_file.GetSize() == 0) {
        LOG_INFO(Render_OpenGL, "No precompiled shader cache found for game with title id={}",
                 GetTitleID());
        return {};
    }

    const auto result = LoadPrecompiledFile(precompiled_file, compressed);
    if (!result) {
        LOG_INFO(Render_OpenGL,
                 "Failed to load precompiled cache for game with title id={} - removing",
                 GetTitleID());
        InvalidatePrecompiled();
        return {};
    }
    return *result;
}

std::optional<std::pair<std::unordered_map<u64, ShaderDiskCacheDecompiled>, ShaderDumpsMap>>
ShaderDiskCache::LoadPrecompiledFile(FileUtil::IOFile& file, bool compressed) {
    // Read compressed file from disk and decompress to virtual precompiled cache file
    const u64 precompiled_size = file.GetSize();
    if (!file.Seek(0, SEEK_SET)) {
#ifdef __SWITCH__
        SwitchShaderDiskCacheLogFile("precompiled.seek-fail", GetProgramID(), separable,
                                     GetPrecompiledPath(), precompiled_size, file.Tell());
#endif
        return std::nullopt;
    }

    std::vector<u8> precompiled_file(precompiled_size);
    file.ReadBytes(precompiled_file.data(), precompiled_file.size());
    if (compressed) {
        const std::vector<u8> decompressed =
            Common::Compression::DecompressDataZSTD(precompiled_file);
        if (decompressed.empty()) {
            LOG_ERROR(Render_OpenGL, "Could not decompress precompiled shader cache.");
            return std::nullopt;
        }
        SaveArrayToPrecompiled(decompressed.data(), decompressed.size());
    } else {
        SaveArrayToPrecompiled(precompiled_file.data(), precompiled_file.size());
    }

    decompressed_precompiled_cache_offset = 0;

    ShaderCacheVersionHash file_hash{};
    if (!LoadArrayFromPrecompiled(file_hash.data(), file_hash.size())) {
        return std::nullopt;
    }
    if (GetShaderCacheVersionHash() != file_hash) {
        LOG_INFO(Render_OpenGL, "Precompiled cache is from another version of the emulator");
        return std::nullopt;
    }

    std::unordered_map<u64, ShaderDiskCacheDecompiled> decompiled;
    ShaderDumpsMap dumps;
    while (decompressed_precompiled_cache_offset < decompressed_precompiled_cache.size()) {
        PrecompiledEntryKind kind{};
        if (!LoadObjectFromPrecompiled(kind)) {
            return std::nullopt;
        }

        switch (kind) {
        case PrecompiledEntryKind::Decompiled: {
            u64 unique_identifier{};
            if (!LoadObjectFromPrecompiled(unique_identifier)) {
                return std::nullopt;
            }

            auto entry = LoadDecompiledEntry();
            if (!entry) {
                return std::nullopt;
            }
            decompiled[unique_identifier] = std::move(*entry);
            break;
        }
        case PrecompiledEntryKind::Dump: {
            u64 unique_identifier;
            if (!LoadObjectFromPrecompiled(unique_identifier)) {
                return std::nullopt;
            }

            ShaderDiskCacheDump dump;
            if (!LoadObjectFromPrecompiled(dump.binary_format)) {
                return std::nullopt;
            }

            u32 binary_length{};
            if (!LoadObjectFromPrecompiled(binary_length)) {
                return std::nullopt;
            }

            dump.binary.resize(binary_length);
            if (!LoadArrayFromPrecompiled(dump.binary.data(), dump.binary.size())) {
                return std::nullopt;
            }

            dumps[unique_identifier] = std::move(dump);
            break;
        }
        default:
            return std::nullopt;
        }
    }

    LOG_INFO(Render_OpenGL,
             "Found a precompiled disk cache with {} decompiled entries and {} binary entries",
             decompiled.size(), dumps.size());
    return {{decompiled, dumps}};
}

std::optional<ShaderDiskCacheDecompiled> ShaderDiskCache::LoadDecompiledEntry() {

    bool sanitize_mul;
    if (!LoadObjectFromPrecompiled(sanitize_mul)) {
        return std::nullopt;
    }

    u32 code_size{};
    if (!LoadObjectFromPrecompiled(code_size)) {
        return std::nullopt;
    }

    std::string code(code_size, '\0');
    if (!LoadArrayFromPrecompiled(code.data(), code.size())) {
        return std::nullopt;
    }

    ShaderDiskCacheDecompiled entry;
    entry.code = std::move(code);
    entry.sanitize_mul = sanitize_mul;

    return entry;
}

void ShaderDiskCache::SaveDecompiledToFile(FileUtil::IOFile& file, u64 unique_identifier,
                                           const std::string& code, bool sanitize_mul) {
    if (!IsUsable())
        return;

    if (file.WriteObject(static_cast<u32>(PrecompiledEntryKind::Decompiled)) != 1 ||
        file.WriteObject(unique_identifier) != 1 || file.WriteObject(sanitize_mul) != 1 ||
        file.WriteObject(static_cast<u32>(code.size())) != 1 ||
        file.WriteArray(code.data(), code.size()) != code.size()) {
        LOG_ERROR(Render_OpenGL, "Failed to save decompiled cache entry - removing");
        file.Close();
        InvalidatePrecompiled();
    }
}

bool ShaderDiskCache::SaveDecompiledToCache(u64 unique_identifier, const std::string& code,
                                            bool sanitize_mul) {
    if (!SaveObjectToPrecompiled(static_cast<u32>(PrecompiledEntryKind::Decompiled)) ||
        !SaveObjectToPrecompiled(unique_identifier) || !SaveObjectToPrecompiled(sanitize_mul) ||
        !SaveObjectToPrecompiled(static_cast<u32>(code.size())) ||
        !SaveArrayToPrecompiled(code.data(), code.size())) {
        return false;
    }

    return true;
}

void ShaderDiskCache::InvalidateAll() {
    transferable_file.Close();
    if (!FileUtil::Delete(GetTransferablePath())) {
        LOG_ERROR(Render_OpenGL, "Failed to invalidate transferable file={}",
                  GetTransferablePath());
    }
    transferable_file = AppendTransferableFile();

    InvalidatePrecompiled();
}

void ShaderDiskCache::InvalidatePrecompiled() {
    // Clear virtual precompiled cache file
    decompressed_precompiled_cache.resize(0);

    precompiled_file.Close();
    if (!FileUtil::Delete(GetPrecompiledPath())) {
        LOG_ERROR(Render_OpenGL, "Failed to invalidate precompiled file={}", GetPrecompiledPath());
    }
    precompiled_file = AppendPrecompiledFile(!separable);
}

void ShaderDiskCache::SaveRaw(const ShaderDiskCacheRaw& entry) {
    if (!IsUsable())
        return;

    const u64 id = entry.GetUniqueIdentifier();
    if (transferable.find(id) != transferable.end()) {
        // The shader already exists
        return;
    }

    if (!transferable_file.Seek(0, SEEK_END)) {
#ifdef __SWITCH__
        SwitchShaderDiskCacheLogFile("transferable.save-seek-fail", GetProgramID(), separable,
                                     GetTransferablePath(), transferable_file.GetSize(),
                                     transferable_file.Tell());
#endif
        LOG_ERROR(Render_OpenGL, "Failed to seek transferable cache for append");
        return;
    }

    if (transferable_file.WriteObject(TransferableEntryKind::Raw) != 1 ||
        !entry.Save(transferable_file)) {
        LOG_ERROR(Render_OpenGL, "Failed to save raw transferable cache entry - removing");
        InvalidateAll();
        return;
    }
    transferable.insert({id, entry});
    transferable_file.Flush();
#ifdef __SWITCH__
    SwitchShaderDiskCacheLogFile("transferable.save-raw", GetProgramID(), separable,
                                 GetTransferablePath(), transferable_file.GetSize(),
                                 transferable_file.Tell());
#endif
}

void ShaderDiskCache::SaveDecompiled(u64 unique_identifier, const std::string& code,
                                     bool sanitize_mul) {
    if (!IsUsable())
        return;

    if (decompressed_precompiled_cache.empty()) {
        SavePrecompiledHeaderToVirtualPrecompiledCache();
    }

    if (!SaveDecompiledToCache(unique_identifier, code, sanitize_mul)) {
        LOG_ERROR(Render_OpenGL,
                  "Failed to save decompiled entry to the precompiled file - removing");
        InvalidatePrecompiled();
    }
}

void ShaderDiskCache::SaveDump(u64 unique_identifier, GLuint program) {
    if (!IsUsable())
        return;

    GLint binary_length{};
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binary_length);

    GLenum binary_format{};
    std::vector<u8> binary(binary_length);
    glGetProgramBinary(program, binary_length, nullptr, &binary_format, binary.data());

    if (!SaveObjectToPrecompiled(static_cast<u32>(PrecompiledEntryKind::Dump)) ||
        !SaveObjectToPrecompiled(unique_identifier) ||
        !SaveObjectToPrecompiled(static_cast<u32>(binary_format)) ||
        !SaveObjectToPrecompiled(static_cast<u32>(binary_length)) ||
        !SaveArrayToPrecompiled(binary.data(), binary.size())) {
        LOG_ERROR(Render_OpenGL, "Failed to save binary program file in shader={:016x} - removing",
                  unique_identifier);
        InvalidatePrecompiled();
        return;
    }
}

void ShaderDiskCache::SaveDumpToFile(u64 unique_identifier, GLuint program, bool sanitize_mul) {
    if (!IsUsable())
        return;

    GLint binary_length{};
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binary_length);

    GLenum binary_format{};
    std::vector<u8> binary(binary_length);
    glGetProgramBinary(program, binary_length, nullptr, &binary_format, binary.data());

    if (precompiled_file.WriteObject(static_cast<u32>(PrecompiledEntryKind::Dump)) != 1 ||
        precompiled_file.WriteObject(unique_identifier) != 1 ||
        precompiled_file.WriteObject(static_cast<u32>(binary_format)) != 1 ||
        precompiled_file.WriteObject(static_cast<u32>(binary_length)) != 1 ||
        precompiled_file.WriteArray(binary.data(), binary.size()) != binary.size()) {
        LOG_ERROR(Render_OpenGL, "Failed to save binary program file in shader={:016x} - removing",
                  unique_identifier);
        InvalidatePrecompiled();
        return;
    }

    // SaveDecompiled is used only to store the accurate multiplication setting, a better way is to
    // probably change the header in SaveDump
    SaveDecompiledToFile(precompiled_file, unique_identifier, {}, sanitize_mul);

    precompiled_file.Flush();
}

bool ShaderDiskCache::IsUsable() const {
    return tried_to_load && Settings::values.use_disk_shader_cache;
}

FileUtil::IOFile ShaderDiskCache::AppendTransferableFile() {
    if (!EnsureDirectories())
        return {};

    const auto transferable_path{GetTransferablePath()};
    const bool existed = FileUtil::Exists(transferable_path);

#ifdef HAVE_LIBRETRO_VFS
    // LibRetro's VFS maps "ab+" to RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, which
    // uses "r+b" internally and fails if the file doesn't exist. Pre-create it.
    if (!existed) {
        FileUtil::CreateEmptyFile(transferable_path);
    }
#endif

    FileUtil::IOFile file(transferable_path, "ab+");
    if (!file.IsOpen()) {
        LOG_ERROR(Render_OpenGL, "Failed to open transferable cache in path={}", transferable_path);
        return {};
    }

    u64 file_size = file.GetSize();
#ifdef __SWITCH__
    SwitchShaderDiskCacheLogFile("transferable.open", GetProgramID(), separable,
                                 transferable_path, file_size, file.Tell());
#endif
    if (file_size == 0) {
        // If the file is empty, write its version.
        if (file.WriteObject(NativeVersion) != 1) {
            LOG_ERROR(Render_OpenGL, "Failed to write transferable cache version in path={}",
                      transferable_path);
            return {};
        }
        file.Flush();
        file_size = file.GetSize();
#ifdef __SWITCH__
        SwitchShaderDiskCacheLogFile("transferable.header-write", GetProgramID(), separable,
                                     transferable_path, file_size, file.Tell());
#endif
    }
    file.Seek(0, SEEK_SET);
    return file;
}

FileUtil::IOFile ShaderDiskCache::AppendPrecompiledFile(bool write_header) {
    if (!EnsureDirectories())
        return {};

    const auto precompiled_path{GetPrecompiledPath()};
    const bool existed = FileUtil::Exists(precompiled_path);

#ifdef HAVE_LIBRETRO_VFS
    // LibRetro's VFS maps "ab+" to RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, which
    // uses "r+b" internally and fails if the file doesn't exist. Pre-create it.
    if (!existed) {
        FileUtil::CreateEmptyFile(precompiled_path);
    }
#endif

    FileUtil::IOFile file(precompiled_path, "ab+");
    if (!file.IsOpen()) {
        LOG_ERROR(Render_OpenGL, "Failed to open precompiled cache in path={}", precompiled_path);
        return {};
    }

    u64 file_size = file.GetSize();
#ifdef __SWITCH__
    SwitchShaderDiskCacheLogFile("precompiled.open", GetProgramID(), separable, precompiled_path,
                                 file_size, file.Tell());
#endif

    // If the file is empty, write its version.
    if (write_header && file_size == 0) {
        const auto hash{GetShaderCacheVersionHash()};
        if (file.WriteArray(hash.data(), hash.size()) != hash.size()) {
            LOG_ERROR(Render_OpenGL, "Failed to write precompiled cache version in path={}",
                      precompiled_path);
            return {};
        }
        file.Flush();
        file_size = file.GetSize();
#ifdef __SWITCH__
        SwitchShaderDiskCacheLogFile("precompiled.header-write", GetProgramID(), separable,
                                     precompiled_path, file_size, file.Tell());
#endif
    }
    file.Seek(0, SEEK_SET);
    return file;
}

void ShaderDiskCache::SavePrecompiledHeaderToVirtualPrecompiledCache() {
    const auto hash{GetShaderCacheVersionHash()};
    if (!SaveArrayToPrecompiled(hash.data(), hash.size())) {
        LOG_ERROR(
            Render_OpenGL,
            "Failed to write precompiled cache version hash to virtual precompiled cache file");
    }
}

void ShaderDiskCache::SaveVirtualPrecompiledFile() {
    decompressed_precompiled_cache_offset = 0;
    const auto compressed =
        Common::Compression::CompressDataZSTDDefault(decompressed_precompiled_cache);

    const auto precompiled_path{GetPrecompiledPath()};

#ifdef HAVE_LIBRETRO_VFS
    const bool existed = FileUtil::Exists(precompiled_path);
    if (!existed) {
        FileUtil::CreateEmptyFile(precompiled_path);
    }
#endif

    precompiled_file.Close();
    if (!FileUtil::Delete(GetPrecompiledPath())) {
        LOG_ERROR(Render_OpenGL, "Failed to invalidate precompiled file={}", GetPrecompiledPath());
    }
    precompiled_file = AppendPrecompiledFile(!separable);

    if (precompiled_file.WriteBytes(compressed.data(), compressed.size()) != compressed.size()) {
        LOG_ERROR(Render_OpenGL, "Failed to write precompiled cache version in path={}",
                  precompiled_path);
        return;
    }

    precompiled_file.Flush();
}

bool ShaderDiskCache::EnsureDirectories() const {
    const auto CreateDir = [](const std::string& dir) {
        if (!FileUtil::CreateDir(dir)) {
            LOG_ERROR(Render_OpenGL, "Failed to create directory={}", dir);
            return false;
        }
        return true;
    };

    return CreateDir(FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir)) &&
           CreateDir(GetBaseDir()) && CreateDir(GetTransferableDir()) &&
           CreateDir(GetPrecompiledDir()) && CreateDir(GetPrecompiledShaderDir());
}

std::string ShaderDiskCache::GetTransferablePath() {
    return FileUtil::SanitizePath(GetTransferableDir() + DIR_SEP_CHR + GetTitleID() + ".bin");
}

std::string ShaderDiskCache::GetPrecompiledPath() {
    return FileUtil::SanitizePath(GetPrecompiledShaderDir() + DIR_SEP_CHR + GetTitleID() + ".bin");
}

std::string ShaderDiskCache::GetTransferableDir() const {
    return GetBaseDir() + DIR_SEP "transferable";
}

std::string ShaderDiskCache::GetPrecompiledDir() const {
    return GetBaseDir() + DIR_SEP "precompiled";
}

std::string ShaderDiskCache::GetPrecompiledShaderDir() const {
    if (separable) {
        return GetPrecompiledDir() + DIR_SEP "separable";
    }
    return GetPrecompiledDir() + DIR_SEP "conventional";
}

std::string ShaderDiskCache::GetBaseDir() const {
    return FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) + DIR_SEP "opengl";
}

u64 ShaderDiskCache::GetProgramID() const {
    return program_id;
}

std::string ShaderDiskCache::GetTitleID() {
    if (!title_id.empty()) {
        return title_id;
    }
    title_id = fmt::format("{:016X}", GetProgramID());
    return title_id;
}

} // namespace OpenGL
