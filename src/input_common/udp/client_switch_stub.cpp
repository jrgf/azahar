// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <utility>

#include "common/logging/log.h"
#include "input_common/udp/client.h"

namespace InputCommon::CemuhookUDP {

class Socket {};

Client::Client(std::shared_ptr<DeviceStatus> status_, const std::string& host, u16 port,
               u8 pad_index, u32 client_id)
    : status(std::move(status_)) {
    (void)host;
    (void)port;
    (void)pad_index;
    (void)client_id;
    LOG_INFO(Input, "UDP input is disabled in the Switch homebrew build");
}

Client::~Client() = default;

void Client::ReloadSocket(const std::string& host, u16 port, u8 pad_index, u32 client_id) {
    (void)host;
    (void)port;
    (void)pad_index;
    (void)client_id;
    LOG_DEBUG(Input, "Ignoring UDP input reload in the Switch homebrew build");
}

void TestCommunication(const std::string& host, u16 port, u8 pad_index, u32 client_id,
                       const std::function<void()>& success_callback,
                       const std::function<void()>& failure_callback) {
    (void)host;
    (void)port;
    (void)pad_index;
    (void)client_id;
    (void)success_callback;
    if (failure_callback) {
        failure_callback();
    }
}

CalibrationConfigurationJob::CalibrationConfigurationJob(
    const std::string& host, u16 port, u8 pad_index, u32 client_id,
    std::function<void(Status)> status_callback,
    std::function<void(u16, u16, u16, u16)> data_callback) {
    (void)host;
    (void)port;
    (void)pad_index;
    (void)client_id;
    (void)data_callback;
    if (status_callback) {
        status_callback(Status::Initialized);
    }
}

CalibrationConfigurationJob::~CalibrationConfigurationJob() {
    Stop();
}

void CalibrationConfigurationJob::Stop() {
    complete_event.Set();
}

} // namespace InputCommon::CemuhookUDP
