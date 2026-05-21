// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

#include "emulator_bootstrap.h"

namespace Azahar::Switch {

enum class CoreBridgeResult {
    Success,
    MissingPath,
    LoaderUnavailable,
    LoadFailed,
    RunLoopFailed,
    ShutdownRequested,
};

struct CoreBridgeStatus {
    CoreBridgeResult result = CoreBridgeResult::LoadFailed;
    char message[192]{};
};

const char* GetCoreBridgeResultName(CoreBridgeResult result);
CoreBridgeStatus StartCoreBridge(const GameCandidate& game);
void StopCoreBridge();

} // namespace Azahar::Switch
