// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "emulation_session.h"

#ifdef AZAHAR_SWITCH_ENABLE_CORE_BRIDGE
#include "core_bridge/core_bridge.h"
#endif

#include <cstdio>

namespace Azahar::Switch {
namespace {

void CopyPath(char* out, const char* path) {
    const char* source = path != nullptr ? path : "";
    std::size_t index = 0;
    for (; index + 1 < MaxPathLength && source[index] != '\0'; ++index) {
        out[index] = source[index];
    }
    out[index] = '\0';
}

void SetStatus(EmulationSession& session, const char* message) {
    std::snprintf(session.status_message, sizeof(session.status_message), "%s",
                  message != nullptr ? message : "");
}

} // namespace

const char* GetEmulationStartResultName(EmulationStartResult result) {
    switch (result) {
    case EmulationStartResult::Started:
        return "started";
    case EmulationStartResult::MissingPath:
        return "missing-path";
    case EmulationStartResult::CoreBridgeUnavailable:
        return "core-bridge-unavailable";
    }
    return "unknown";
}

EmulationStartResult StartEmulation(EmulationSession& session, const GameCandidate& game) {
    if (game.path[0] == '\0') {
        SetStatus(session, "No bootable file selected");
        AppendLogFormat(nullptr, "android-flow result=missing-path");
        return EmulationStartResult::MissingPath;
    }

    session.running = true;
    session.stop_requested = false;
    CopyPath(session.active_path, game.path);

    AppendLogFormat(nullptr, "azahar-start path=\"%s\" type=\"%s\"", game.path,
                    GetFileTypeName(game.type, game.compressed));
    AppendLogFormat(nullptr, "android-flow stage=validate-path result=ok");
    AppendLogFormat(nullptr, "android-flow stage=create-emu-window result=pending platform=switch");

    const LoaderProbe probe = ProbeGameFile(game);
    AppendLogFormat(nullptr, "android-flow stage=loader result=%s extension-type=\"%s\" "
                             "detected-type=\"%s\" magic=%s errno=%d path=\"%s\"",
                    GetLoaderProbeResultName(probe.result),
                    GetFileTypeName(game.type, game.compressed),
                    GetFileTypeName(probe.detected_type, false), probe.magic, probe.file_errno,
                    game.path);

    if (probe.result != LoaderProbeResult::Ok &&
        probe.result != LoaderProbeResult::Unsupported) {
        session.running = false;
        SetStatus(session, "Loader probe failed");
        return EmulationStartResult::CoreBridgeUnavailable;
    }

#ifdef AZAHAR_SWITCH_ENABLE_CORE_BRIDGE
    const CoreBridgeStatus core_status = StartCoreBridge(game);
    session.running = false;
    SetStatus(session, core_status.message);
    return core_status.result == CoreBridgeResult::Success ? EmulationStartResult::Started
                                                           : EmulationStartResult::CoreBridgeUnavailable;
#else
    AppendLogFormat(nullptr,
                    "android-flow stage=system-load result=blocked missing=switch-emu-window,citra-core-target");

    session.running = false;
    SetStatus(session, "Core bridge pending: Switch EmuWindow + citra_core target");
    return EmulationStartResult::CoreBridgeUnavailable;
#endif
}

void StopEmulation(EmulationSession& session) {
    if (!session.running) {
        return;
    }

    session.stop_requested = true;
    session.running = false;
#ifdef AZAHAR_SWITCH_ENABLE_CORE_BRIDGE
    StopCoreBridge();
#endif
    AppendLogFormat(nullptr, "azahar-stop path=\"%s\"", session.active_path);
    SetStatus(session, "Stopped");
}

} // namespace Azahar::Switch
