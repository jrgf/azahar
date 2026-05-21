// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <csignal>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/optimization_flags.h>
#include "common/assert.h"
#include "common/microprofile.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_cp15.h"
#include "core/arm/dynarmic/arm_exclusive_monitor.h"
#include "core/arm/dynarmic/arm_tick_counts.h"
#include "core/core.h"
#include "core/core_timing.h"
#ifdef ENABLE_GDBSTUB
#include "core/gdbstub/gdbstub.h"
#endif
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

#ifdef __SWITCH__
namespace Azahar::Switch {
bool AppendLogFormat(int* error_out, const char* format, ...);
}

extern "C" void DynarmicSwitchGetJitStats(std::uint64_t* out, std::size_t len);

namespace {
enum SwitchCallbackStat : std::size_t {
    ReadCount,
    WriteCount,
    ExclusiveCount,
    SvcCount,
    SvcTotalMs,
    SvcMaxMs,
    SvcLast,
    ExceptionCount,
    AddTicksCount,
    AddTicksTotal,
    TicksRemainingCount,
    TicksForCodeCount,
    CodeReadCount,
    Count,
};

std::atomic<std::uint64_t> switch_callback_stats[SwitchCallbackStat::Count]{};

std::uint64_t SwitchDynarmicElapsedMs(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              start)
            .count());
}

void SwitchCallbackStatAdd(SwitchCallbackStat stat, std::uint64_t value = 1) {
    switch_callback_stats[stat].fetch_add(value, std::memory_order_relaxed);
}

void SwitchCallbackStatMax(SwitchCallbackStat stat, std::uint64_t candidate) {
    auto current = switch_callback_stats[stat].load(std::memory_order_relaxed);
    while (current < candidate &&
           !switch_callback_stats[stat].compare_exchange_weak(current, candidate,
                                                              std::memory_order_relaxed)) {
    }
}

void SwitchCallbackStatsSnapshot(std::uint64_t* out, std::size_t len) {
    if (out == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < len && i < SwitchCallbackStat::Count; ++i) {
        out[i] = switch_callback_stats[i].load(std::memory_order_relaxed);
    }
}
} // namespace
#endif

#ifndef SIGILL
constexpr u32 SIGILL = 4;
#endif

#ifndef SIGTRAP
constexpr u32 SIGTRAP = 5;
#endif

namespace Core {

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit DynarmicUserCallbacks(ARM_Dynarmic& parent)
        : parent(parent), svc_context(parent.system), memory(parent.memory) {}
    ~DynarmicUserCallbacks() = default;

    std::optional<std::uint32_t> MemoryReadCode(VAddr vaddr) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(CodeReadCount);
#endif
        return memory.Read32OrNullopt(vaddr);
    }

    std::uint8_t MemoryRead8(VAddr vaddr) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(ReadCount);
#endif
        return memory.Read8(vaddr);
    }
    std::uint16_t MemoryRead16(VAddr vaddr) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(ReadCount);
#endif
        return memory.Read16(vaddr);
    }
    std::uint32_t MemoryRead32(VAddr vaddr) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(ReadCount);
#endif
        return memory.Read32(vaddr);
    }
    std::uint64_t MemoryRead64(VAddr vaddr) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(ReadCount);
#endif
        return memory.Read64(vaddr);
    }

    void MemoryWrite8(VAddr vaddr, std::uint8_t value) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(WriteCount);
#endif
        memory.Write8(vaddr, value);
    }
    void MemoryWrite16(VAddr vaddr, std::uint16_t value) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(WriteCount);
#endif
        memory.Write16(vaddr, value);
    }
    void MemoryWrite32(VAddr vaddr, std::uint32_t value) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(WriteCount);
#endif
        memory.Write32(vaddr, value);
    }
    void MemoryWrite64(VAddr vaddr, std::uint64_t value) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(WriteCount);
#endif
        memory.Write64(vaddr, value);
    }

    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(ExclusiveCount);
#endif
        return memory.WriteExclusive8(vaddr, value, expected);
    }
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(ExclusiveCount);
#endif
        return memory.WriteExclusive16(vaddr, value, expected);
    }
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(ExclusiveCount);
#endif
        return memory.WriteExclusive32(vaddr, value, expected);
    }
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(ExclusiveCount);
#endif
        return memory.WriteExclusive64(vaddr, value, expected);
    }

    void InterpreterFallback(VAddr pc, std::size_t num_instructions) override {
        // Should never happen.
        UNREACHABLE_MSG("InterpeterFallback reached with pc = 0x{:08x}, code = 0x{:08x}, num = {}",
                        pc, MemoryReadCode(pc).value(), num_instructions);
    }

    void CallSVC(std::uint32_t swi) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(SvcCount);
        switch_callback_stats[SvcLast].store(swi, std::memory_order_relaxed);
        const auto switch_svc_start = std::chrono::steady_clock::now();
#endif
        svc_context.CallSVC(swi);
#ifdef __SWITCH__
        const auto switch_svc_ms = SwitchDynarmicElapsedMs(switch_svc_start);
        SwitchCallbackStatAdd(SvcTotalMs, switch_svc_ms);
        SwitchCallbackStatMax(SvcMaxMs, switch_svc_ms);
#endif
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(ExceptionCount);
#endif
        switch (exception) {
        case Dynarmic::A32::Exception::UndefinedInstruction:
        case Dynarmic::A32::Exception::UnpredictableInstruction:
        case Dynarmic::A32::Exception::DecodeError:
        case Dynarmic::A32::Exception::NoExecuteFault:
            break;
        case Dynarmic::A32::Exception::Breakpoint:
#ifdef ENABLE_GDBSTUB
            if (GDBStub::IsConnected()) {
                parent.SetPC(pc);
                parent.ServeBreak(SIGTRAP);
                return;
            }
#endif
            break;
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForInterrupt:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadInstruction:
            return;
        }

        static constexpr auto ExceptionToString = [](Dynarmic::A32::Exception e) -> std::string {
            switch (e) {
            case Dynarmic::A32::Exception::UndefinedInstruction:
                return "UndefinedInstruction";
            case Dynarmic::A32::Exception::UnpredictableInstruction:
                return "UnpredictableInstruction";
            case Dynarmic::A32::Exception::DecodeError:
                return "DecodeError";
            case Dynarmic::A32::Exception::NoExecuteFault:
                return "NoExecuteFault";
            case Dynarmic::A32::Exception::Breakpoint:
                return "Breakpoint";
            default:
                return fmt::format("Unknown({})", e);
            }
        };

        parent.SetPC(pc);
#ifdef ENABLE_GDBSTUB
        if (GDBStub::IsConnected()) {
            parent.ServeBreak(SIGILL);
        } else
#endif
        {
            std::string error;
            for (int i = 0; i < 16; i++) {
                error += fmt::format("r{:02d} = {:08X}\n", i, parent.GetReg(i));
            }
            error += fmt::format("ExceptionRaised(exception = {}, pc = {:08X})",
                                 ExceptionToString(exception), pc);
            parent.system.SetStatus(Core::System::ResultStatus::ErrorCoreExceptionRaised,
                                    error.c_str());
        }
    }

    void AddTicks(std::uint64_t ticks) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(AddTicksCount);
        SwitchCallbackStatAdd(AddTicksTotal, ticks);
#endif
        parent.GetTimer().AddTicks(ticks);
    }
    std::uint64_t GetTicksRemaining() override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(TicksRemainingCount);
#endif
        s64 ticks = parent.GetTimer().GetDowncount();
        return static_cast<u64>(ticks <= 0 ? 0 : ticks);
    }
    std::uint64_t GetTicksForCode(bool is_thumb, VAddr, std::uint32_t instruction) override {
#ifdef __SWITCH__
        SwitchCallbackStatAdd(TicksForCodeCount);
#endif
        return Core::TicksForInstruction(is_thumb, instruction);
    }

    ARM_Dynarmic& parent;
    Kernel::SVCContext svc_context;
    Memory::MemorySystem& memory;
};

ARM_Dynarmic::ARM_Dynarmic(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id_,
                           std::shared_ptr<Core::Timing::Timer> timer_,
                           Core::ExclusiveMonitor& exclusive_monitor_)
    : ARM_Interface(core_id_, timer_), system(system_), memory(memory_),
      cb(std::make_unique<DynarmicUserCallbacks>(*this)),
      exclusive_monitor{dynamic_cast<Core::DynarmicExclusiveMonitor&>(exclusive_monitor_)} {
    SetPageTable(memory.GetCurrentPageTable());
}

ARM_Dynarmic::~ARM_Dynarmic() = default;

MICROPROFILE_DEFINE(ARM_Jit, "ARM JIT", "ARM JIT", MP_RGB(255, 64, 64));

void ARM_Dynarmic::Run() {
    ASSERT(memory.GetCurrentPageTable() == current_page_table);
    MICROPROFILE_SCOPE(ARM_Jit);
    if (break_flag) [[unlikely]] {
        return;
    }

#ifdef __SWITCH__
    static unsigned switch_run_trace_samples = 0;
    const bool switch_trace_run = switch_run_trace_samples++ < 32;
    std::uint64_t switch_stats_before[12]{};
    std::uint64_t switch_cb_before[SwitchCallbackStat::Count]{};
    decltype(GetPC()) switch_pc_before{};
    decltype(GetTimer().GetTicks()) switch_ticks_before{};
    decltype(GetTimer().GetDowncount()) switch_downcount_before{};
    if (switch_trace_run) {
        DynarmicSwitchGetJitStats(switch_stats_before, 12);
        SwitchCallbackStatsSnapshot(switch_cb_before, SwitchCallbackStat::Count);
        switch_pc_before = GetPC();
        switch_ticks_before = GetTimer().GetTicks();
        switch_downcount_before = GetTimer().GetDowncount();
    }
    const auto switch_run_start = switch_trace_run ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
#endif
    jit->Run();
#ifdef __SWITCH__
    const auto switch_run_ms = switch_trace_run ? SwitchDynarmicElapsedMs(switch_run_start) : 0;
    static unsigned switch_slow_run_logs = 0;
    if (switch_trace_run && switch_run_ms >= 500 && switch_slow_run_logs < 32) {
        std::uint64_t switch_stats_after[12]{};
        std::uint64_t switch_cb_after[SwitchCallbackStat::Count]{};
        DynarmicSwitchGetJitStats(switch_stats_after, 12);
        SwitchCallbackStatsSnapshot(switch_cb_after, SwitchCallbackStat::Count);
        const auto switch_pc_after = GetPC();
        const auto switch_ticks_after = GetTimer().GetTicks();
        const auto switch_downcount_after = GetTimer().GetDowncount();
        ++switch_slow_run_logs;
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=dynarmic.run.slow core=%u elapsed-ms=%llu emits=%llu "
            "emit-ms=%llu emit-max-ms=%llu emit-arm64-ms=%llu link-ms=%llu relink-ms=%llu "
            "invalidate-ms=%llu protect-ms=%llu clear=%llu bytes=%llu max-bytes=%llu "
            "pc-before=%08X pc-after=%08X ticks-before=%lld ticks-after=%lld "
            "down-before=%lld down-after=%lld cb-read=%llu cb-write=%llu cb-exclusive=%llu "
            "cb-svc=%llu cb-svc-ms=%llu cb-svc-max-ms=%llu cb-svc-last=%llu cb-exception=%llu "
            "cb-addticks=%llu cb-addticks-total=%llu cb-ticks-remaining=%llu "
            "cb-ticks-code=%llu cb-code-read=%llu",
            GetID(), static_cast<unsigned long long>(switch_run_ms),
            static_cast<unsigned long long>(switch_stats_after[0] - switch_stats_before[0]),
            static_cast<unsigned long long>(switch_stats_after[3] - switch_stats_before[3]),
            static_cast<unsigned long long>(switch_stats_after[4]),
            static_cast<unsigned long long>(switch_stats_after[5] - switch_stats_before[5]),
            static_cast<unsigned long long>(switch_stats_after[6] - switch_stats_before[6]),
            static_cast<unsigned long long>(switch_stats_after[7] - switch_stats_before[7]),
            static_cast<unsigned long long>(switch_stats_after[8] - switch_stats_before[8]),
            static_cast<unsigned long long>(switch_stats_after[9] - switch_stats_before[9]),
            static_cast<unsigned long long>(switch_stats_after[2] - switch_stats_before[2]),
            static_cast<unsigned long long>(switch_stats_after[10] - switch_stats_before[10]),
            static_cast<unsigned long long>(switch_stats_after[11]), switch_pc_before,
            switch_pc_after, static_cast<long long>(switch_ticks_before),
            static_cast<long long>(switch_ticks_after), static_cast<long long>(switch_downcount_before),
            static_cast<long long>(switch_downcount_after),
            static_cast<unsigned long long>(switch_cb_after[ReadCount] - switch_cb_before[ReadCount]),
            static_cast<unsigned long long>(switch_cb_after[WriteCount] -
                                            switch_cb_before[WriteCount]),
            static_cast<unsigned long long>(switch_cb_after[ExclusiveCount] -
                                            switch_cb_before[ExclusiveCount]),
            static_cast<unsigned long long>(switch_cb_after[SvcCount] - switch_cb_before[SvcCount]),
            static_cast<unsigned long long>(switch_cb_after[SvcTotalMs] -
                                            switch_cb_before[SvcTotalMs]),
            static_cast<unsigned long long>(switch_cb_after[SvcMaxMs]),
            static_cast<unsigned long long>(switch_cb_after[SvcLast]),
            static_cast<unsigned long long>(switch_cb_after[ExceptionCount] -
                                            switch_cb_before[ExceptionCount]),
            static_cast<unsigned long long>(switch_cb_after[AddTicksCount] -
                                            switch_cb_before[AddTicksCount]),
            static_cast<unsigned long long>(switch_cb_after[AddTicksTotal] -
                                            switch_cb_before[AddTicksTotal]),
            static_cast<unsigned long long>(switch_cb_after[TicksRemainingCount] -
                                            switch_cb_before[TicksRemainingCount]),
            static_cast<unsigned long long>(switch_cb_after[TicksForCodeCount] -
                                            switch_cb_before[TicksForCodeCount]),
            static_cast<unsigned long long>(switch_cb_after[CodeReadCount] -
                                            switch_cb_before[CodeReadCount]));
    }
#endif
}

void ARM_Dynarmic::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }

    jit->Step();
}

void ARM_Dynarmic::SetPC(u32 pc) {
    jit->Regs()[15] = pc;
}

u32 ARM_Dynarmic::GetPC() const {
    return jit->Regs()[15];
}

u32 ARM_Dynarmic::GetReg(int index) const {
    return jit->Regs()[index];
}

void ARM_Dynarmic::SetReg(int index, u32 value) {
    jit->Regs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPReg(int index) const {
    return jit->ExtRegs()[index];
}

void ARM_Dynarmic::SetVFPReg(int index, u32 value) {
    jit->ExtRegs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return jit->Fpscr();
    case VFP_FPEXC:
        return fpexc;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }

    return UINT_MAX;
}

void ARM_Dynarmic::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        jit->SetFpscr(value);
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

u32 ARM_Dynarmic::GetCPSR() const {
    return jit->Cpsr();
}

void ARM_Dynarmic::SetCPSR(u32 cpsr) {
    jit->SetCpsr(cpsr);
}

u32 ARM_Dynarmic::GetCP15Register(CP15Register reg) const {
    switch (reg) {
    case CP15_THREAD_UPRW:
        return cp15_state.cp15_thread_uprw;
    case CP15_THREAD_URO:
        return cp15_state.cp15_thread_uro;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }

    return 0;
}

void ARM_Dynarmic::SetCP15Register(CP15Register reg, u32 value) {
    switch (reg) {
    case CP15_THREAD_UPRW:
        cp15_state.cp15_thread_uprw = value;
        return;
    case CP15_THREAD_URO:
        cp15_state.cp15_thread_uro = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }
}

void ARM_Dynarmic::SaveContext(ThreadContext& ctx) {
    ctx.cpu_registers = jit->Regs();
    ctx.cpsr = jit->Cpsr();
    ctx.fpu_registers = jit->ExtRegs();
    ctx.fpscr = jit->Fpscr();
    ctx.fpexc = fpexc;
}

void ARM_Dynarmic::LoadContext(const ThreadContext& ctx) {
    jit->Regs() = ctx.cpu_registers;
    jit->SetCpsr(ctx.cpsr);
    jit->ExtRegs() = ctx.fpu_registers;
    jit->SetFpscr(ctx.fpscr);
    fpexc = ctx.fpexc;
}

void ARM_Dynarmic::PrepareReschedule() {
    if (jit->IsExecuting()) {
        jit->HaltExecution();
    }
}

void ARM_Dynarmic::ClearInstructionCache() {
    for (const auto& j : jits) {
        j.second->ClearCache();
    }
}

void ARM_Dynarmic::InvalidateCacheRange(u32 start_address, std::size_t length) {
    jit->InvalidateCacheRange(start_address, length);
}

void ARM_Dynarmic::ClearExclusiveState() {
    jit->ClearExclusiveState();
}

std::shared_ptr<Memory::PageTable> ARM_Dynarmic::GetPageTable() const {
    return current_page_table;
}

void ARM_Dynarmic::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    current_page_table = page_table;
    ThreadContext ctx{};
    if (jit) {
        SaveContext(ctx);
    }

    auto iter = jits.find(current_page_table);
    if (iter != jits.end()) {
        jit = iter->second.get();
        LoadContext(ctx);
        return;
    }

    auto new_jit = MakeJit();
    jit = new_jit.get();
    LoadContext(ctx);
    jits.emplace(current_page_table, std::move(new_jit));
}

void ARM_Dynarmic::ServeBreak([[maybe_unused]] int signal) {
#ifdef ENABLE_GDBSTUB
    GDBStub::Break(signal);
#endif
}

std::unique_ptr<Dynarmic::A32::Jit> ARM_Dynarmic::MakeJit() {
    Dynarmic::A32::UserConfig config;
    config.callbacks = cb.get();
    if (current_page_table) {
        config.page_table = &current_page_table->GetPointerArray();
    }
    config.coprocessors[15] = std::make_shared<DynarmicCP15>(cp15_state);
    config.define_unpredictable_behaviour = true;

    // Multi-process state
    config.processor_id = GetID();
    config.global_monitor = &exclusive_monitor.monitor;

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

} // namespace Core
