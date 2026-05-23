// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "emulator_bootstrap.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <ctime>
#include <sys/stat.h>

namespace Azahar::Switch {
namespace {

constexpr const char* AppDirectory = "sdmc:/switch/azahar";
constexpr const char* LogPath = "sdmc:/switch/azahar/log.txt";
constexpr const char* RomDirectory = "sdmc:/switch/azahar/roms";

FILE* g_log_file = nullptr;
char g_log_buffer[64 * 1024];
unsigned g_log_line_count = 0;

bool ShouldFlushLogLine(const char* format) {
    if (format == nullptr) {
        return true;
    }
    return g_log_line_count < 16 || std::strstr(format, "shutdown") != nullptr ||
           std::strstr(format, "fatal") != nullptr || std::strstr(format, "crash") != nullptr ||
           std::strstr(format, "libretro-build-marker") != nullptr ||
           std::strstr(format, "stage=process.") != nullptr ||
           std::strstr(format, "stage=libretro.") != nullptr ||
           std::strstr(format, "stage=deko3d.stack") != nullptr ||
           std::strstr(format, "stage=deko3d.present-") != nullptr ||
           std::strstr(format, "stage=deko3d.frame-submit") != nullptr ||
           std::strstr(format, "stage=deko3d.pica-target") != nullptr ||
           std::strstr(format, "stage=deko3d-rasterizer.cpu-vs-batch") != nullptr ||
           std::strstr(format, "stage=switch-opengl.") != nullptr ||
           std::strstr(format, "stage=switch-cpu-boost") != nullptr ||
           std::strstr(format, "stage=switch-thread-priority") != nullptr ||
           std::strstr(format, "stage=libretro.run") != nullptr;
}

bool IsDirectory(const char* path) {
    struct stat status {};
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

bool EnsureDirectory(const char* path, int* error_out) {
    if (error_out != nullptr) {
        *error_out = 0;
    }

    if (IsDirectory(path)) {
        return true;
    }

    errno = 0;
    if (mkdir(path, 0777) == 0) {
        return true;
    }

    if (errno == EEXIST && IsDirectory(path)) {
        return true;
    }

    if (error_out != nullptr) {
        *error_out = errno;
    }
    return false;
}

bool ExtensionEquals(const char* extension, const char* expected) {
    if (extension == nullptr || expected == nullptr) {
        return false;
    }

    while (*extension != '\0' && *expected != '\0') {
        const auto lhs = static_cast<unsigned char>(*extension++);
        const auto rhs = static_cast<unsigned char>(*expected++);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return *extension == '\0' && *expected == '\0';
}

bool MagicEquals(const unsigned char* data, char a, char b, char c, char d) {
    return data[0] == static_cast<unsigned char>(a) && data[1] == static_cast<unsigned char>(b) &&
           data[2] == static_cast<unsigned char>(c) && data[3] == static_cast<unsigned char>(d);
}

const char* GetExtension(const char* path) {
    const char* extension = std::strrchr(path, '.');
    return extension != nullptr ? extension : "";
}

GameFileType GuessFileType(const char* path, bool* compressed_out) {
    const char* extension = GetExtension(path);
    const bool compressed = extension[0] == '.' && extension[1] == 'z';

    if (compressed_out != nullptr) {
        *compressed_out = compressed;
    }

    if (ExtensionEquals(extension, ".elf") || ExtensionEquals(extension, ".axf")) {
        return GameFileType::ELF;
    }
    if (ExtensionEquals(extension, ".cci") || ExtensionEquals(extension, ".zcci") ||
        ExtensionEquals(extension, ".3ds")) {
        return GameFileType::CCI;
    }
    if (ExtensionEquals(extension, ".cxi") || ExtensionEquals(extension, ".app") ||
        ExtensionEquals(extension, ".zcxi")) {
        return GameFileType::CXI;
    }
    if (ExtensionEquals(extension, ".3dsx") || ExtensionEquals(extension, ".z3dsx")) {
        return GameFileType::THREEDSX;
    }
    if (ExtensionEquals(extension, ".cia") || ExtensionEquals(extension, ".zcia")) {
        return GameFileType::CIA;
    }

    return GameFileType::Unknown;
}

void CopyString(char* out, std::size_t out_size, const char* value) {
    if (out_size == 0) {
        return;
    }

    const char* source = value != nullptr ? value : "";
    std::strncpy(out, source, out_size - 1);
    out[out_size - 1] = '\0';
}

void SortCatalog(GameCatalog& catalog) {
    std::sort(catalog.entries, catalog.entries + catalog.count,
              [](const GameCandidate& lhs, const GameCandidate& rhs) {
                  return std::strcmp(lhs.name, rhs.name) < 0;
              });
}

} // namespace

const char* GetAppDirectory() {
    return AppDirectory;
}

const char* GetLogPath() {
    return LogPath;
}

const char* GetRomDirectory() {
    return RomDirectory;
}

bool EnsureAppDirectories(int* error_out) {
    if (!EnsureDirectory(AppDirectory, error_out)) {
        return false;
    }
    return EnsureDirectory(RomDirectory, error_out);
}

bool AppendLogFormat(int* error_out, const char* format, ...) {
    if (error_out != nullptr) {
        *error_out = 0;
    }

    if (g_log_file == nullptr) {
        g_log_file = std::fopen(LogPath, "w");
        if (g_log_file != nullptr) {
            std::setvbuf(g_log_file, g_log_buffer, _IOFBF, sizeof(g_log_buffer));
        }
    }
    if (g_log_file == nullptr) {
        if (error_out != nullptr) {
            *error_out = errno;
        }
        return false;
    }

    const std::time_t now = std::time(nullptr);
    std::fprintf(g_log_file, "%lld ", static_cast<long long>(now));

    va_list args;
    va_start(args, format);
    std::vfprintf(g_log_file, format, args);
    va_end(args);

    std::fprintf(g_log_file, "\n");
    ++g_log_line_count;
    if ((g_log_line_count % 64) == 0 || ShouldFlushLogLine(format)) {
        std::fflush(g_log_file);
    }
    return true;
}

void FlushLog() {
    if (g_log_file != nullptr) {
        std::fflush(g_log_file);
    }
}

GameCatalog ScanGameDirectory() {
    GameCatalog catalog{};

    DIR* directory = opendir(RomDirectory);
    if (directory == nullptr) {
        catalog.scan_errno = errno;
        return catalog;
    }

    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char path[MaxPathLength]{};
        if (std::snprintf(path, sizeof(path), "%s/%s", RomDirectory, entry->d_name) >=
            static_cast<int>(sizeof(path))) {
            catalog.truncated = true;
            continue;
        }

        struct stat status {};
        if (stat(path, &status) != 0 || S_ISDIR(status.st_mode)) {
            continue;
        }

        bool compressed = false;
        const GameFileType type = GuessFileType(path, &compressed);
        if (type == GameFileType::Unknown) {
            continue;
        }

        if (catalog.count >= MaxGameCandidates) {
            catalog.truncated = true;
            continue;
        }

        GameCandidate& candidate = catalog.entries[catalog.count++];
        CopyString(candidate.path, sizeof(candidate.path), path);
        CopyString(candidate.name, sizeof(candidate.name), entry->d_name);
        candidate.type = type;
        candidate.compressed = compressed;
        candidate.size = static_cast<std::uint64_t>(status.st_size);
    }

    closedir(directory);
    SortCatalog(catalog);
    return catalog;
}

void MoveSelection(const GameCatalog& catalog, int& selected_index, int delta) {
    if (catalog.count == 0) {
        selected_index = 0;
        return;
    }

    selected_index += delta;
    if (selected_index < 0) {
        selected_index = static_cast<int>(catalog.count) - 1;
    } else if (selected_index >= static_cast<int>(catalog.count)) {
        selected_index = 0;
    }
}

const char* GetFileTypeName(GameFileType type, bool compressed) {
    switch (type) {
    case GameFileType::CCI:
        return compressed ? "NCSD (Z)" : "NCSD";
    case GameFileType::CXI:
        return compressed ? "NCCH (Z)" : "NCCH";
    case GameFileType::CIA:
        return compressed ? "CIA (Z)" : "CIA";
    case GameFileType::ELF:
        return "ELF";
    case GameFileType::THREEDSX:
        return compressed ? "3DSX (Z)" : "3DSX";
    case GameFileType::Unknown:
    default:
        return "unknown";
    }
}

const char* GetLoaderProbeResultName(LoaderProbeResult result) {
    switch (result) {
    case LoaderProbeResult::Ok:
        return "ok";
    case LoaderProbeResult::OpenFailed:
        return "open-failed";
    case LoaderProbeResult::ReadFailed:
        return "read-failed";
    case LoaderProbeResult::MagicMismatch:
        return "magic-mismatch";
    case LoaderProbeResult::Unsupported:
        return "unsupported";
    }
    return "unknown";
}

LoaderProbe ProbeGameFile(const GameCandidate& game) {
    LoaderProbe probe{};

    if (game.compressed || game.type == GameFileType::CIA) {
        probe.result = LoaderProbeResult::Unsupported;
        probe.detected_type = game.type;
        CopyString(probe.magic, sizeof(probe.magic), game.compressed ? "zfile" : "cia");
        return probe;
    }

    FILE* file = std::fopen(game.path, "rb");
    if (file == nullptr) {
        probe.result = LoaderProbeResult::OpenFailed;
        probe.file_errno = errno;
        return probe;
    }

    unsigned char magic[4]{};
    std::size_t read_size = 0;

    if (game.type == GameFileType::ELF || game.type == GameFileType::THREEDSX) {
        read_size = std::fread(magic, 1, sizeof(magic), file);
    } else if (game.type == GameFileType::CCI || game.type == GameFileType::CXI) {
        if (std::fseek(file, 0x100, SEEK_SET) == 0) {
            read_size = std::fread(magic, 1, sizeof(magic), file);
        }
    }

    std::fclose(file);

    if (read_size != sizeof(magic)) {
        probe.result = LoaderProbeResult::ReadFailed;
        probe.file_errno = errno;
        return probe;
    }

    std::snprintf(probe.magic, sizeof(probe.magic), "%02X%02X%02X%02X", magic[0], magic[1],
                  magic[2], magic[3]);

    if (MagicEquals(magic, '\x7f', 'E', 'L', 'F')) {
        probe.detected_type = GameFileType::ELF;
    } else if (MagicEquals(magic, '3', 'D', 'S', 'X')) {
        probe.detected_type = GameFileType::THREEDSX;
    } else if (MagicEquals(magic, 'N', 'C', 'S', 'D')) {
        probe.detected_type = GameFileType::CCI;
    } else if (MagicEquals(magic, 'N', 'C', 'C', 'H')) {
        probe.detected_type = GameFileType::CXI;
    } else {
        probe.detected_type = GameFileType::Unknown;
    }

    probe.result = probe.detected_type == GameFileType::Unknown ? LoaderProbeResult::MagicMismatch
                                                                : LoaderProbeResult::Ok;
    return probe;
}

} // namespace Azahar::Switch
