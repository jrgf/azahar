// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/service/soc/soc_u.h"

SERIALIZE_EXPORT_IMPL(Service::SOC::SOC_U)

namespace Service::SOC {

namespace {
void PushResult(Kernel::HLERequestContext& ctx, Result result) {
    IPC::RequestBuilder rb(ctx, 1, 0);
    rb.Push(result);
}
} // namespace

SOC_U::SOC_U() : ServiceFramework("soc:U", 18) {
    static const FunctionInfo functions[] = {
        // clang-format off
        {0x0001, &SOC_U::InitializeSockets, "InitializeSockets"},
        {0x0002, &SOC_U::StubNetworkUnavailable, "Socket"},
        {0x0003, &SOC_U::StubNetworkUnavailable, "Bind"},
        {0x0004, &SOC_U::StubNetworkUnavailable, "Fcntl"},
        {0x0005, &SOC_U::StubNetworkUnavailable, "Listen"},
        {0x0006, &SOC_U::StubNetworkUnavailable, "Accept"},
        {0x0007, &SOC_U::StubNetworkUnavailable, "SockAtMark"},
        {0x0008, &SOC_U::StubNetworkUnavailable, "GetHostId"},
        {0x0009, &SOC_U::StubNetworkUnavailable, "Close"},
        {0x000A, &SOC_U::StubNetworkUnavailable, "SendToOther"},
        {0x000B, &SOC_U::StubNetworkUnavailable, "SendToSingle"},
        {0x000C, &SOC_U::StubNetworkUnavailable, "RecvFromOther"},
        {0x000D, &SOC_U::StubNetworkUnavailable, "RecvFrom"},
        {0x000E, &SOC_U::StubNetworkUnavailable, "Poll"},
        {0x000F, &SOC_U::StubNetworkUnavailable, "GetSockName"},
        {0x0010, &SOC_U::StubNetworkUnavailable, "Shutdown"},
        {0x0011, &SOC_U::StubNetworkUnavailable, "GetHostByAddr"},
        {0x0012, &SOC_U::StubNetworkUnavailable, "GetHostByName"},
        {0x0013, &SOC_U::StubNetworkUnavailable, "GetPeerName"},
        {0x0014, &SOC_U::StubNetworkUnavailable, "Connect"},
        {0x0015, &SOC_U::StubNetworkUnavailable, "GetSockOpt"},
        {0x0016, &SOC_U::StubNetworkUnavailable, "SetSockOpt"},
        {0x0017, &SOC_U::StubNetworkUnavailable, "GetNetworkOpt"},
        {0x0018, &SOC_U::StubNetworkUnavailable, "GetAddrInfo"},
        {0x0019, &SOC_U::ShutdownSockets, "ShutdownSockets"},
        {0x001A, &SOC_U::StubNetworkUnavailable, "GetNameInfo"},
        {0x001B, nullptr, "ICMPSocket"},
        {0x001C, &SOC_U::StubNetworkUnavailable, "ICMPPing"},
        {0x001D, &SOC_U::StubNetworkUnavailable, "ICMPCancel"},
        {0x001E, &SOC_U::StubNetworkUnavailable, "ICMPClose"},
        {0x001F, &SOC_U::StubNetworkUnavailable, "GetResolverInfo"},
        {0x0020, &SOC_U::StubNetworkUnavailable, "SendToMultiple"},
        {0x0021, &SOC_U::CloseSockets, "CloseSockets"},
        {0x0023, &SOC_U::AddGlobalSocket, "AddGlobalSocket"},
        // clang-format on
    };
    RegisterHandlers(functions);
}

SOC_U::~SOC_U() = default;

std::optional<SOC_U::InterfaceInfo> SOC_U::GetDefaultInterfaceInfo() {
    return std::nullopt;
}

void SOC_U::InitializeSockets(Kernel::HLERequestContext& ctx) {
    PushResult(ctx, ResultSuccess);
}

void SOC_U::ShutdownSockets(Kernel::HLERequestContext& ctx) {
    PushResult(ctx, ResultSuccess);
}

void SOC_U::CloseSockets(Kernel::HLERequestContext& ctx) {
    PushResult(ctx, ResultSuccess);
}

void SOC_U::AddGlobalSocket(Kernel::HLERequestContext& ctx) {
    StubNetworkUnavailable(ctx);
}

void SOC_U::StubNetworkUnavailable(Kernel::HLERequestContext& ctx) {
    LOG_WARNING(Service_SOC, "SOC service is disabled in the Switch homebrew build");
    PushResult(ctx, ResultNotInitialized);
}

std::shared_ptr<SOC_U> GetService(Core::System& system) {
    return system.ServiceManager().GetService<SOC_U>("soc:U");
}

void InstallInterfaces(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    std::make_shared<SOC_U>()->InstallAsService(service_manager);
}

} // namespace Service::SOC
