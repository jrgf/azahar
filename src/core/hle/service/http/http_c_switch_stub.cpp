// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/result.h"
#include "core/hle/service/http/http_c.h"

SERIALIZE_EXPORT_IMPL(Service::HTTP::HTTP_C)
SERIALIZE_EXPORT_IMPL(Service::HTTP::SessionData)

namespace Service::HTTP {

namespace {
constexpr Result ErrorNetworkUnavailable =
    Result(1012, ErrorModule::HTTP, ErrorSummary::Internal, ErrorLevel::Permanent);

void PushResult(Kernel::HLERequestContext& ctx, Result result) {
    IPC::RequestBuilder rb(ctx, 1, 0);
    rb.Push(result);
}
} // namespace

void HTTP_C::Initialize(Kernel::HLERequestContext& ctx) {
    PushResult(ctx, ResultSuccess);
}

void HTTP_C::InitializeConnectionSession(Kernel::HLERequestContext& ctx) {
    PushResult(ctx, ResultSuccess);
}

void HTTP_C::Finalize(Kernel::HLERequestContext& ctx) {
    PushResult(ctx, ResultSuccess);
}

void HTTP_C::StubNetworkUnavailable(Kernel::HLERequestContext& ctx) {
    LOG_WARNING(Service_HTTP, "HTTP service is disabled in the Switch homebrew build");
    PushResult(ctx, ErrorNetworkUnavailable);
}

HTTP_C::HTTP_C() : ServiceFramework("http:C", 32) {
    static const FunctionInfo functions[] = {
        // clang-format off
        {0x0001, &HTTP_C::Initialize, "Initialize"},
        {0x0002, &HTTP_C::StubNetworkUnavailable, "CreateContext"},
        {0x0003, &HTTP_C::StubNetworkUnavailable, "CloseContext"},
        {0x0004, &HTTP_C::StubNetworkUnavailable, "CancelConnection"},
        {0x0005, &HTTP_C::StubNetworkUnavailable, "GetRequestState"},
        {0x0006, &HTTP_C::StubNetworkUnavailable, "GetDownloadSizeState"},
        {0x0007, &HTTP_C::StubNetworkUnavailable, "GetRequestError"},
        {0x0008, &HTTP_C::InitializeConnectionSession, "InitializeConnectionSession"},
        {0x0009, &HTTP_C::StubNetworkUnavailable, "BeginRequest"},
        {0x000A, &HTTP_C::StubNetworkUnavailable, "BeginRequestAsync"},
        {0x000B, &HTTP_C::StubNetworkUnavailable, "ReceiveData"},
        {0x000C, &HTTP_C::StubNetworkUnavailable, "ReceiveDataTimeout"},
        {0x000D, &HTTP_C::StubNetworkUnavailable, "SetProxy"},
        {0x000E, &HTTP_C::StubNetworkUnavailable, "SetProxyDefault"},
        {0x000F, &HTTP_C::StubNetworkUnavailable, "SetBasicAuthorization"},
        {0x0010, &HTTP_C::StubNetworkUnavailable, "SetSocketBufferSize"},
        {0x0011, &HTTP_C::StubNetworkUnavailable, "AddRequestHeader"},
        {0x0012, &HTTP_C::StubNetworkUnavailable, "AddPostDataAscii"},
        {0x0013, &HTTP_C::StubNetworkUnavailable, "AddPostDataBinary"},
        {0x0014, &HTTP_C::StubNetworkUnavailable, "AddPostDataRaw"},
        {0x0015, &HTTP_C::StubNetworkUnavailable, "SetPostDataType"},
        {0x0016, &HTTP_C::StubNetworkUnavailable, "SendPostDataAscii"},
        {0x0017, &HTTP_C::StubNetworkUnavailable, "SendPostDataAsciiTimeout"},
        {0x0018, &HTTP_C::StubNetworkUnavailable, "SendPostDataBinary"},
        {0x0019, &HTTP_C::StubNetworkUnavailable, "SendPostDataBinaryTimeout"},
        {0x001A, &HTTP_C::StubNetworkUnavailable, "SendPostDataRaw"},
        {0x001B, &HTTP_C::StubNetworkUnavailable, "SendPostDataRawTimeout"},
        {0x001C, &HTTP_C::StubNetworkUnavailable, "SetPostDataEncoding"},
        {0x001D, &HTTP_C::StubNetworkUnavailable, "NotifyFinishSendPostData"},
        {0x001E, &HTTP_C::StubNetworkUnavailable, "GetResponseHeader"},
        {0x001F, &HTTP_C::StubNetworkUnavailable, "GetResponseHeaderTimeout"},
        {0x0020, &HTTP_C::StubNetworkUnavailable, "GetResponseData"},
        {0x0021, &HTTP_C::StubNetworkUnavailable, "GetResponseDataTimeout"},
        {0x0022, &HTTP_C::StubNetworkUnavailable, "GetResponseStatusCode"},
        {0x0023, &HTTP_C::StubNetworkUnavailable, "GetResponseStatusCodeTimeout"},
        {0x0024, &HTTP_C::StubNetworkUnavailable, "AddTrustedRootCA"},
        {0x0025, &HTTP_C::StubNetworkUnavailable, "AddDefaultCert"},
        {0x0026, &HTTP_C::StubNetworkUnavailable, "SelectRootCertChain"},
        {0x0027, &HTTP_C::StubNetworkUnavailable, "SetClientCert"},
        {0x0028, &HTTP_C::StubNetworkUnavailable, "SetDefaultClientCert"},
        {0x0029, &HTTP_C::StubNetworkUnavailable, "SetClientCertContext"},
        {0x002A, &HTTP_C::StubNetworkUnavailable, "GetSSLError"},
        {0x002B, &HTTP_C::StubNetworkUnavailable, "SetSSLOpt"},
        {0x002C, &HTTP_C::StubNetworkUnavailable, "SetSSLClearOpt"},
        {0x002D, &HTTP_C::StubNetworkUnavailable, "CreateRootCertChain"},
        {0x002E, &HTTP_C::StubNetworkUnavailable, "DestroyRootCertChain"},
        {0x002F, &HTTP_C::StubNetworkUnavailable, "RootCertChainAddCert"},
        {0x0030, &HTTP_C::StubNetworkUnavailable, "RootCertChainAddDefaultCert"},
        {0x0031, &HTTP_C::StubNetworkUnavailable, "RootCertChainRemoveCert"},
        {0x0032, &HTTP_C::StubNetworkUnavailable, "OpenClientCertContext"},
        {0x0033, &HTTP_C::StubNetworkUnavailable, "OpenDefaultClientCertContext"},
        {0x0034, &HTTP_C::StubNetworkUnavailable, "CloseClientCertContext"},
        {0x0035, &HTTP_C::StubNetworkUnavailable, "SetDefaultProxy"},
        {0x0036, &HTTP_C::StubNetworkUnavailable, "ClearDNSCache"},
        {0x0037, &HTTP_C::StubNetworkUnavailable, "SetKeepAlive"},
        {0x0038, &HTTP_C::StubNetworkUnavailable, "SetPostDataTypeSize"},
        {0x0039, &HTTP_C::Finalize, "Finalize"},
        // clang-format on
    };
    RegisterHandlers(functions);
}

std::shared_ptr<HTTP_C> GetService(Core::System& system) {
    return system.ServiceManager().GetService<HTTP_C>("http:C");
}

void InstallInterfaces(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    std::make_shared<HTTP_C>()->InstallAsService(service_manager);
}

} // namespace Service::HTTP
