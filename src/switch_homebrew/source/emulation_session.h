// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

#include "emulator_bootstrap.h"

namespace Azahar::Switch {

enum class EmulationStartResult {
    Started,
    MissingPath,
    CoreBridgeUnavailable,
};

struct EmulationSession {
    bool running = false;
    bool stop_requested = false;
    char active_path[MaxPathLength]{};
    char status_message[192]{};
};

const char* GetEmulationStartResultName(EmulationStartResult result);

EmulationStartResult StartEmulation(EmulationSession& session, const GameCandidate& game);
void StopEmulation(EmulationSession& session);

} // namespace Azahar::Switch
