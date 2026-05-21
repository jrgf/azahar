// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#include "switch_jit_probe.h"

#include "emulator_bootstrap.h"

#include <array>
#include <cstdint>
#include <switch.h>

namespace Azahar::Switch {

bool RunSwitchJitProbe() {
    AppendLogFormat(nullptr, "android-flow stage=switch-jit-probe.begin");

    Jit jit{};
    constexpr std::size_t CodeSize = 0x1000;
    Result result = jitCreate(&jit, CodeSize);
    AppendLogFormat(nullptr,
                    "android-flow stage=switch-jit-probe.create result=%08X rw=%p rx=%p size=%zu",
                    result, jit.rw_addr, jit.rx_addr, CodeSize);
    if (R_FAILED(result)) {
        return false;
    }

    result = jitTransitionToWritable(&jit);
    AppendLogFormat(nullptr, "android-flow stage=switch-jit-probe.writable result=%08X", result);
    if (R_FAILED(result)) {
        jitClose(&jit);
        return false;
    }

    auto* code = static_cast<std::uint32_t*>(jit.rw_addr);
    // mov w0, #42; ret
    code[0] = 0x52800540;
    code[1] = 0xD65F03C0;
    armDCacheClean(jit.rw_addr, 8);
    armICacheInvalidate(jit.rx_addr, 8);
    AppendLogFormat(nullptr, "android-flow stage=switch-jit-probe.cache-flush");

    result = jitTransitionToExecutable(&jit);
    AppendLogFormat(nullptr, "android-flow stage=switch-jit-probe.executable result=%08X",
                    result);
    if (R_FAILED(result)) {
        jitClose(&jit);
        return false;
    }

    AppendLogFormat(nullptr, "android-flow stage=switch-jit-probe.call begin");
    using ProbeFunction = int (*)();
    const int value = reinterpret_cast<ProbeFunction>(jit.rx_addr)();
    AppendLogFormat(nullptr, "android-flow stage=switch-jit-probe.call result=%d", value);

    result = jitClose(&jit);
    AppendLogFormat(nullptr, "android-flow stage=switch-jit-probe.close result=%08X", result);
    return value == 42 && R_SUCCEEDED(result);
}

} // namespace Azahar::Switch
