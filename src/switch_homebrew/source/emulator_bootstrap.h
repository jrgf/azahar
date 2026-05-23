// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <cstdint>

namespace Azahar::Switch {

constexpr std::size_t MaxGameCandidates = 32;
constexpr std::size_t MaxPathLength = 512;
constexpr std::size_t MaxNameLength = 160;

enum class GameFileType {
    Unknown,
    CCI,
    CXI,
    CIA,
    ELF,
    THREEDSX,
};

struct GameCandidate {
    char path[MaxPathLength]{};
    char name[MaxNameLength]{};
    GameFileType type = GameFileType::Unknown;
    bool compressed = false;
    std::uint64_t size = 0;
};

struct GameCatalog {
    GameCandidate entries[MaxGameCandidates]{};
    std::size_t count = 0;
    bool truncated = false;
    int scan_errno = 0;
};

enum class LoaderProbeResult {
    Ok,
    OpenFailed,
    ReadFailed,
    MagicMismatch,
    Unsupported,
};

struct LoaderProbe {
    LoaderProbeResult result = LoaderProbeResult::Unsupported;
    GameFileType detected_type = GameFileType::Unknown;
    char magic[16]{};
    int file_errno = 0;
};

const char* GetAppDirectory();
const char* GetLogPath();
const char* GetRomDirectory();

bool EnsureAppDirectories(int* error_out);
bool AppendLogFormat(int* error_out, const char* format, ...);
void FlushLog();
GameCatalog ScanGameDirectory();
void MoveSelection(const GameCatalog& catalog, int& selected_index, int delta);

const char* GetFileTypeName(GameFileType type, bool compressed);
const char* GetLoaderProbeResultName(LoaderProbeResult result);
LoaderProbe ProbeGameFile(const GameCandidate& game);

} // namespace Azahar::Switch
