// Copyright 2020 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <string>

#include "common/logging/log.h"
#include "core/nus_download.h"

namespace Core::NUS {

std::optional<std::vector<u8>> Download(const std::string& path) {
    LOG_WARNING(WebService, "NUS downloads are disabled in the Switch homebrew build: {}", path);
    return std::nullopt;
}

} // namespace Core::NUS
