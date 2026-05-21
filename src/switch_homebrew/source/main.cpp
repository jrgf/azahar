// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "emulator_bootstrap.h"
#include "emulation_session.h"
#include "switch_input.h"

#include <switch.h>

#include <cstdio>

namespace {

using Azahar::Switch::GameCatalog;
using Azahar::Switch::EmulationSession;

struct RuntimeState {
    bool mount_attempted = false;
    Result mount_result = 0;
    bool storage_ready = false;
    bool directories_ready = false;
    bool log_ready = false;
    int directory_errno = 0;
    int log_errno = 0;
    int frame_count = 0;
    int selected_game = 0;
    char status_message[192]{};
};

const char* BoolText(bool value) {
    return value ? "ok" : "failed";
}

RuntimeState InitializeRuntime() {
    RuntimeState state{};

    if (fsdevGetDeviceFileSystem("sdmc") == nullptr) {
        state.mount_attempted = true;
        state.mount_result = fsdevMountSdmc();
    }

    state.storage_ready = fsdevGetDeviceFileSystem("sdmc") != nullptr;
    state.directories_ready = Azahar::Switch::EnsureAppDirectories(&state.directory_errno);

    state.log_ready = Azahar::Switch::AppendLogFormat(&state.log_errno, "launch");
    std::snprintf(state.status_message, sizeof(state.status_message), "Ready");

    return state;
}

void PrintGameList(const GameCatalog& catalog, int selected_game) {
    if (catalog.scan_errno != 0) {
        std::printf("ROM scan failed: errno %d\n", catalog.scan_errno);
        return;
    }

    if (catalog.count == 0) {
        std::printf("No bootable files found.\n");
        std::printf("Place games or homebrew under:\n");
        std::printf("  %s\n", Azahar::Switch::GetRomDirectory());
        return;
    }

    const int count = static_cast<int>(catalog.count);
    int first = selected_game - 3;
    if (first < 0) {
        first = 0;
    }
    if (first > count - 7) {
        first = count > 7 ? count - 7 : 0;
    }

    for (int index = first; index < count && index < first + 7; ++index) {
        const auto& game = catalog.entries[index];
        std::printf("%c %-28.28s %s\n", index == selected_game ? '>' : ' ', game.name,
                    Azahar::Switch::GetFileTypeName(game.type, game.compressed));
    }

    if (catalog.truncated) {
        std::printf("\nList truncated to %zu entries.\n", Azahar::Switch::MaxGameCandidates);
    }
}

void PrintStatus(const RuntimeState& state, const GameCatalog& catalog,
                 const EmulationSession& emulation_session) {
    std::printf("\x1b[2J\x1b[H");
    std::printf("Azahar Switch Emulator\n");
    std::printf("----------------------\n\n");
    std::printf("Storage: %s  App dirs: %s  Log: %s\n", BoolText(state.storage_ready),
                BoolText(state.directories_ready), BoolText(state.log_ready));
    std::printf("ROMs: %zu  Frame: %d\n", catalog.count, state.frame_count);
    std::printf("Status: %s\n\n", state.status_message);
    if (emulation_session.status_message[0] != '\0') {
        std::printf("Core: %s\n\n", emulation_session.status_message);
    }

    PrintGameList(catalog, state.selected_game);

    std::printf("\nControls: Up/Down select  A start  X rescan  + exit\n");
}

void RequestBoot(RuntimeState& state, EmulationSession& emulation_session,
                 const GameCatalog& catalog) {
    if (catalog.count == 0 || state.selected_game >= static_cast<int>(catalog.count)) {
        std::snprintf(state.status_message, sizeof(state.status_message),
                      "No bootable file selected");
        return;
    }

    const auto& game = catalog.entries[state.selected_game];
    const auto result = Azahar::Switch::StartEmulation(emulation_session, game);
    consoleInit(nullptr);
    Azahar::Switch::AppendLogFormat(nullptr, "android-flow stage=switch-console result=restored");
    std::snprintf(state.status_message, sizeof(state.status_message),
                  "Start result: %s", Azahar::Switch::GetEmulationStartResultName(result));
}

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    consoleInit(nullptr);
    Azahar::Switch::RegisterSwitchInput();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    RuntimeState state = InitializeRuntime();
    EmulationSession emulation_session{};
    GameCatalog catalog = Azahar::Switch::ScanGameDirectory();
    Azahar::Switch::AppendLogFormat(nullptr, "runtime-ready games=%zu", catalog.count);

    while (appletMainLoop()) {
        padUpdate(&pad);

        const u64 down_buttons = padGetButtonsDown(&pad);

        if ((down_buttons & HidNpadButton_Plus) != 0) {
            Azahar::Switch::StopEmulation(emulation_session);
            Azahar::Switch::AppendLogFormat(nullptr, "exit-requested");
            break;
        }
        if ((down_buttons & HidNpadButton_AnyUp) != 0) {
            Azahar::Switch::MoveSelection(catalog, state.selected_game, -1);
        }
        if ((down_buttons & HidNpadButton_AnyDown) != 0) {
            Azahar::Switch::MoveSelection(catalog, state.selected_game, 1);
        }
        if ((down_buttons & HidNpadButton_X) != 0) {
            catalog = Azahar::Switch::ScanGameDirectory();
            state.selected_game = 0;
            Azahar::Switch::AppendLogFormat(nullptr, "rescan games=%zu", catalog.count);
            std::snprintf(state.status_message, sizeof(state.status_message), "Rescanned ROMs");
        }
        if ((down_buttons & HidNpadButton_A) != 0) {
            RequestBoot(state, emulation_session, catalog);
        }

        ++state.frame_count;
        PrintStatus(state, catalog, emulation_session);
        consoleUpdate(nullptr);
    }

    Azahar::Switch::AppendLogFormat(nullptr, "shutdown");

    if (state.mount_attempted && R_SUCCEEDED(state.mount_result)) {
        fsdevUnmountAll();
    }

    consoleExit(nullptr);
    return 0;
}
