// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef __SWITCH__
#include <atomic>
#include <chrono>
#endif

#include "common/arch.h"
#include "common/archives.h"
#include "common/microprofile.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/vertex_loader.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/shader/shader.h"

#ifdef __SWITCH__
namespace Azahar::Switch {
bool AppendLogFormat(int* error_out, const char* format, ...);
}
#endif

namespace Pica {

MICROPROFILE_DEFINE(GPU_Drawing, "GPU", "Drawing", MP_RGB(50, 50, 240));

using namespace DebugUtils;

#ifdef __SWITCH__
namespace {
std::atomic<unsigned> switch_pica_trace_logs{};
constexpr unsigned SwitchPicaTraceLogLimit = 64;

bool SwitchPicaReserveLog() {
    return switch_pica_trace_logs.fetch_add(1, std::memory_order_relaxed) < SwitchPicaTraceLogLimit;
}

long long SwitchPicaElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

bool SwitchPicaShouldLogSlow(long long elapsed_ms) {
    return elapsed_ms >= 5;
}

const char* SwitchPicaRegName(u32 id) {
    switch (id) {
    case PICA_REG_INDEX(irq_request):
        return "irq_request";
    case PICA_REG_INDEX(pipeline.trigger_draw):
        return "trigger_draw";
    case PICA_REG_INDEX(pipeline.trigger_draw_indexed):
        return "trigger_draw_indexed";
    case PICA_REG_INDEX(pipeline.command_buffer.trigger[0]):
        return "cmd_buffer_trigger0";
    case PICA_REG_INDEX(pipeline.command_buffer.trigger[1]):
        return "cmd_buffer_trigger1";
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.index):
        return "vs_default_attr_index";
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[0]):
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[1]):
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[2]):
        return "vs_default_attr_value";
    case PICA_REG_INDEX(pipeline.num_vertices):
        return "num_vertices";
    case PICA_REG_INDEX(pipeline.vertex_offset):
        return "vertex_offset";
    default:
        return "reg";
    }
}
} // namespace
#endif

union CommandHeader {
    u32 hex;
    BitField<0, 16, u32> cmd_id;
    BitField<16, 4, u32> parameter_mask;
    BitField<20, 8, u32> extra_data_length;
    BitField<31, 1, u32> group_commands;
};
static_assert(sizeof(CommandHeader) == sizeof(u32), "CommandHeader has incorrect size!");

PicaCore::PicaCore(Memory::MemorySystem& memory_, std::shared_ptr<DebugContext> debug_context_)
    : memory{memory_}, debug_context{std::move(debug_context_)},
      geometry_pipeline{regs.internal, gs_unit, gs_setup},
      shader_engine{CreateEngine(Settings::values.use_shader_jit.GetValue())} {
    InitializeRegs();
    dirty_regs.SetAllDirty();

    const auto submit_vertex = [this](const AttributeBuffer& buffer) {
        const auto add_triangle = [this](const OutputVertex& v0, const OutputVertex& v1,
                                         const OutputVertex& v2) {
            rasterizer->AddTriangle(v0, v1, v2);
        };
        const auto vertex = OutputVertex(regs.internal.rasterizer, buffer);
        primitive_assembler.SubmitVertex(vertex, add_triangle);
    };

    gs_unit.SetVertexHandlers(submit_vertex, [this]() { primitive_assembler.SetWinding(); });
    geometry_pipeline.SetVertexHandler(submit_vertex);

    primitive_assembler.Reconfigure(PipelineRegs::TriangleTopology::List);
}

PicaCore::~PicaCore() = default;

void PicaCore::InitializeRegs() {
    // Values initialized by GSP
    regs.internal.irq_autostop = 1;
    regs.internal.irq_mask = 0xFFFFFFF0;

    auto& framebuffer_top = regs.framebuffer_config[0];
    auto& framebuffer_sub = regs.framebuffer_config[1];

    // Set framebuffer defaults from nn::gx::Initialize
    framebuffer_top.address_left1 = 0x181E6000;
    framebuffer_top.address_left2 = 0x1822C800;
    framebuffer_top.address_right1 = 0x18273000;
    framebuffer_top.address_right2 = 0x182B9800;
    framebuffer_sub.address_left1 = 0x1848F000;
    framebuffer_sub.address_left2 = 0x184C7800;

    framebuffer_top.width.Assign(240);
    framebuffer_top.height.Assign(400);
    framebuffer_top.stride = 3 * 240;
    framebuffer_top.color_format.Assign(PixelFormat::RGB8);
    framebuffer_top.active_fb = 0;

    framebuffer_sub.width.Assign(240);
    framebuffer_sub.height.Assign(320);
    framebuffer_sub.stride = 3 * 240;
    framebuffer_sub.color_format.Assign(PixelFormat::RGB8);
    framebuffer_sub.active_fb = 0;

    // Tales of Abyss expects this register to have the following default values.
    auto& gs = regs.internal.gs;
    gs.max_input_attribute_index.Assign(1);
    gs.shader_mode.Assign(ShaderRegs::ShaderMode::VS);
}

void PicaCore::BindRasterizer(VideoCore::RasterizerInterface* rasterizer) {
    this->rasterizer = rasterizer;
}

void PicaCore::SetInterruptHandler(Service::GSP::InterruptHandler& signal_interrupt) {
    this->signal_interrupt = signal_interrupt;
}

void PicaCore::ProcessCmdList(PAddr list, u32 size, bool ignore_list) {
#ifdef __SWITCH__
    const bool switch_trace_list = SwitchPicaReserveLog();
    const auto switch_list_start =
        switch_trace_list ? std::chrono::steady_clock::now()
                          : std::chrono::steady_clock::time_point{};
#endif
    if (ignore_list) {
        signal_interrupt(Service::GSP::InterruptId::P3D, delay_generator.CalculateAndResetDelay());
#ifdef __SWITCH__
        const auto switch_list_ms =
            switch_trace_list ? SwitchPicaElapsedMs(switch_list_start) : 0;
        if (switch_trace_list && SwitchPicaShouldLogSlow(switch_list_ms)) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=pica.list ignored=1 list=%08X size=%u elapsed-ms=%lld", list,
                size, switch_list_ms);
        }
#endif
        return;
    }
    // Initialize command list tracking.
    const u8* head = memory.GetPhysicalPointer(list);
    cmd_list.Reset(list, head, size);

    bool stop_requested = false;
    while (cmd_list.current_index < cmd_list.length) {
        if (stop_requested) [[unlikely]] {
            break;
        }
        // Align read pointer to 8 bytes
        if (cmd_list.current_index % 2 != 0) {
            cmd_list.current_index++;
        }

        // Read the header and the value to write.
        const u32 command_index = cmd_list.current_index;
        const u32 value = cmd_list.head[cmd_list.current_index++];
        const CommandHeader header{cmd_list.head[cmd_list.current_index++]};

        // Write to the requested PICA register.
#ifdef __SWITCH__
        const auto switch_reg_start =
            switch_trace_list ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
        const PAddr switch_reg_list = cmd_list.addr;
        const u32 switch_reg_length = cmd_list.length;
#endif
        WriteInternalReg(header.cmd_id, value, header.parameter_mask, stop_requested);
#ifdef __SWITCH__
        const auto switch_reg_ms = switch_trace_list ? SwitchPicaElapsedMs(switch_reg_start) : 0;
        if (switch_trace_list && SwitchPicaShouldLogSlow(switch_reg_ms)) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=pica.reg.slow list=%08X size=%u index=%u cmd=%03X "
                "name=%s value=%08X mask=%X extra=0 elapsed-ms=%lld stop=%u current=%u "
                "length=%u",
                switch_reg_list, switch_reg_length * static_cast<u32>(sizeof(u32)), command_index,
                header.cmd_id, SwitchPicaRegName(header.cmd_id), value, header.parameter_mask,
                switch_reg_ms, stop_requested ? 1U : 0U, cmd_list.current_index, cmd_list.length);
        }
#endif

        // Write any extra paramters as well.
        for (u32 i = 0; i < header.extra_data_length; ++i) {
            if (stop_requested) [[unlikely]] {
                break;
            }
            const u32 cmd = header.cmd_id + (header.group_commands ? i + 1 : 0);
            const u32 extra_index = cmd_list.current_index;
            const u32 extra_value = cmd_list.head[cmd_list.current_index++];
#ifdef __SWITCH__
            const auto switch_extra_start =
                switch_trace_list ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
            const PAddr switch_extra_list = cmd_list.addr;
            const u32 switch_extra_length = cmd_list.length;
#endif
            WriteInternalReg(cmd, extra_value, header.parameter_mask, stop_requested);
#ifdef __SWITCH__
            const auto switch_extra_ms =
                switch_trace_list ? SwitchPicaElapsedMs(switch_extra_start) : 0;
            if (switch_trace_list && SwitchPicaShouldLogSlow(switch_extra_ms)) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=pica.reg.slow list=%08X size=%u index=%u cmd=%03X "
                    "name=%s value=%08X mask=%X extra=%u elapsed-ms=%lld stop=%u current=%u "
                    "length=%u",
                    switch_extra_list, switch_extra_length * static_cast<u32>(sizeof(u32)),
                    extra_index, cmd, SwitchPicaRegName(cmd), extra_value, header.parameter_mask,
                    i + 1, switch_extra_ms, stop_requested ? 1U : 0U, cmd_list.current_index,
                    cmd_list.length);
            }
#endif
        }
    }
#ifdef __SWITCH__
    const auto switch_list_ms = switch_trace_list ? SwitchPicaElapsedMs(switch_list_start) : 0;
    if (switch_trace_list && SwitchPicaShouldLogSlow(switch_list_ms)) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=pica.list ignored=0 list=%08X size=%u elapsed-ms=%lld stop=%u "
            "final-list=%08X current=%u length=%u",
            list, size, switch_list_ms, stop_requested ? 1U : 0U, cmd_list.addr,
            cmd_list.current_index, cmd_list.length);
    }
#endif
}

static bool any_byte_match(u32 a, u32 b) {
    return ((a & 0xFF) == (b & 0xFF)) || (((a >> 8) & 0xFF) == ((b >> 8) & 0xFF)) ||
           (((a >> 16) & 0xFF) == ((b >> 16) & 0xFF)) || (((a >> 24) & 0xFF) == ((b >> 24) & 0xFF));
}

void PicaCore::WriteInternalReg(u32 id, u32 value, u32 mask, bool& stop_requested) {
    if (id >= RegsInternal::NUM_REGS) {
        LOG_ERROR(
            HW_GPU,
            "Commandlist tried to write to invalid register 0x{:03X} (value: {:08X}, mask: {:X})",
            id, value, mask);
        return;
    }

    delay_generator.AddCommands(1);

    // Expand a 4-bit mask to 4-byte mask, e.g. 0b0101 -> 0x00FF00FF
    constexpr std::array<u32, 16> ExpandBitsToBytes = {
        0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff,
        0x00ffff00, 0x00ffffff, 0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff,
        0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
    };

    // TODO: Figure out how register masking acts on e.g. vs.uniform_setup.set_value
    const u32 old_value = regs.internal.reg_array[id];
    const u32 write_mask = ExpandBitsToBytes[mask];
    regs.internal.reg_array[id] = (old_value & ~write_mask) | (value & write_mask);

    // Track register write.
    DebugUtils::OnPicaRegWrite(id, mask, regs.internal.reg_array[id]);

    // Track events.
    if (debug_context) {
        debug_context->OnEvent(DebugContext::Event::PicaCommandLoaded, &id);
    }

    switch (id) {
    // Trigger IRQ
    case PICA_REG_INDEX(irq_request):
        // TODO(PabloMK7): This logic is not fully accurate, but close enough:
        // https://problemkaputt.de/gbatek-3ds-gpu-internal-registers-finalize-interrupt-registers.htm
        if (any_byte_match(regs.internal.reg_array[id], regs.internal.irq_compare)) [[likely]] {
            signal_interrupt(Service::GSP::InterruptId::P3D,
                             delay_generator.CalculateAndResetDelay());
            if (regs.internal.irq_autostop) [[likely]] {
                stop_requested = true;
            }
        }
        break;

    case PICA_REG_INDEX(pipeline.triangle_topology):
        primitive_assembler.Reconfigure(regs.internal.pipeline.triangle_topology);
        break;

    case PICA_REG_INDEX(pipeline.restart_primitive):
        primitive_assembler.Reset();
        break;

    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.index):
        immediate.Reset();
        break;

    // Load default vertex input attributes
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[0]):
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[1]):
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[2]):
        SubmitImmediate(value);
        break;

    case PICA_REG_INDEX(pipeline.gpu_mode):
        // This register likely just enables vertex processing and doesn't need any special handling
        break;

    case PICA_REG_INDEX(pipeline.command_buffer.trigger[0]):
    case PICA_REG_INDEX(pipeline.command_buffer.trigger[1]): {
        const u32 index = static_cast<u32>(id - PICA_REG_INDEX(pipeline.command_buffer.trigger[0]));
        const PAddr addr = regs.internal.pipeline.command_buffer.GetPhysicalAddress(index);
        const u32 size = regs.internal.pipeline.command_buffer.GetSize(index);
#ifdef __SWITCH__
        if (SwitchPicaReserveLog()) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=pica.cmd-buffer.trigger id=%03X index=%u addr=%08X size=%u "
                "parent-list=%08X parent-current=%u parent-length=%u",
                id, index, addr, size, cmd_list.addr, cmd_list.current_index, cmd_list.length);
        }
#endif
        const u8* head = memory.GetPhysicalPointer(addr);
        cmd_list.Reset(addr, head, size);
        break;
    }

    // It seems like these trigger vertex rendering
    case PICA_REG_INDEX(pipeline.trigger_draw):
    case PICA_REG_INDEX(pipeline.trigger_draw_indexed): {
        const bool is_indexed = (id == PICA_REG_INDEX(pipeline.trigger_draw_indexed));
#ifdef __SWITCH__
        const bool switch_trace_draw = SwitchPicaReserveLog();
        const u32 switch_vertices = regs.internal.pipeline.num_vertices;
        const u32 switch_vertex_offset = regs.internal.pipeline.vertex_offset;
        if (switch_trace_draw && cmd_list.length <= 16) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=pica.draw.begin id=%03X name=%s indexed=%u value=%08X "
                "vertices=%u vertex-offset=%u list=%08X current=%u length=%u",
                id, SwitchPicaRegName(id), is_indexed ? 1U : 0U, value, switch_vertices,
                switch_vertex_offset, cmd_list.addr, cmd_list.current_index, cmd_list.length);
        }
        const auto switch_draw_start =
            switch_trace_draw ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
#endif
        DrawArrays(is_indexed);
#ifdef __SWITCH__
        const auto switch_draw_ms =
            switch_trace_draw ? SwitchPicaElapsedMs(switch_draw_start) : 0;
        if (switch_trace_draw && SwitchPicaShouldLogSlow(switch_draw_ms)) {
            Azahar::Switch::AppendLogFormat(
                nullptr,
                "android-flow stage=pica.draw.end id=%03X name=%s indexed=%u elapsed-ms=%lld "
                "vertices=%u vertex-offset=%u list=%08X current=%u length=%u",
                id, SwitchPicaRegName(id), is_indexed ? 1U : 0U, switch_draw_ms,
                switch_vertices, switch_vertex_offset, cmd_list.addr, cmd_list.current_index,
                cmd_list.length);
        }
#endif
        break;
    }

    case PICA_REG_INDEX(gs.bool_uniforms):
        gs_setup.WriteUniformBoolReg(regs.internal.gs.bool_uniforms.Value());
        break;

    case PICA_REG_INDEX(gs.int_uniforms[0]):
    case PICA_REG_INDEX(gs.int_uniforms[1]):
    case PICA_REG_INDEX(gs.int_uniforms[2]):
    case PICA_REG_INDEX(gs.int_uniforms[3]): {
        const u32 index = (id - PICA_REG_INDEX(gs.int_uniforms[0]));
        gs_setup.WriteUniformIntReg(index, regs.internal.gs.GetIntUniform(index));
        break;
    }

    case PICA_REG_INDEX(gs.uniform_setup.set_value[0]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[1]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[2]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[3]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[4]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[5]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[6]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[7]): {
        gs_setup.WriteUniformFloatReg(regs.internal.gs, value);
        break;
    }

    case PICA_REG_INDEX(gs.program.set_word[0]):
    case PICA_REG_INDEX(gs.program.set_word[1]):
    case PICA_REG_INDEX(gs.program.set_word[2]):
    case PICA_REG_INDEX(gs.program.set_word[3]):
    case PICA_REG_INDEX(gs.program.set_word[4]):
    case PICA_REG_INDEX(gs.program.set_word[5]):
    case PICA_REG_INDEX(gs.program.set_word[6]):
    case PICA_REG_INDEX(gs.program.set_word[7]): {
        u32& offset = regs.internal.gs.program.offset;
        if (offset >= 4096) {
            LOG_ERROR(HW_GPU, "Invalid GS program offset {}", offset);
        } else {
            gs_setup.UpdateProgramCode(offset, value);
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[0]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[1]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[2]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[3]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[4]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[5]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[6]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[7]): {
        u32& offset = regs.internal.gs.swizzle_patterns.offset;
        if (offset >= gs_setup.GetSwizzleData().size()) {
            LOG_ERROR(HW_GPU, "Invalid GS swizzle pattern offset {}", offset);
        } else {
            gs_setup.UpdateSwizzleData(offset, value);
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(vs.output_mask):
        if (!regs.internal.pipeline.gs_unit_exclusive_configuration &&
            regs.internal.pipeline.use_gs == PipelineRegs::UseGS::No) {
            regs.internal.gs.output_mask.Assign(value);
        }
        break;

    case PICA_REG_INDEX(vs.bool_uniforms):
        vs_setup.WriteUniformBoolReg(regs.internal.vs.bool_uniforms.Value());
        if (!regs.internal.pipeline.gs_unit_exclusive_configuration &&
            regs.internal.pipeline.use_gs == PipelineRegs::UseGS::No) {
            gs_setup.WriteUniformBoolReg(regs.internal.vs.bool_uniforms.Value());
        }
        break;

    case PICA_REG_INDEX(vs.int_uniforms[0]):
    case PICA_REG_INDEX(vs.int_uniforms[1]):
    case PICA_REG_INDEX(vs.int_uniforms[2]):
    case PICA_REG_INDEX(vs.int_uniforms[3]): {
        const u32 index = (id - PICA_REG_INDEX(vs.int_uniforms[0]));
        vs_setup.WriteUniformIntReg(index, regs.internal.vs.GetIntUniform(index));
        if (!regs.internal.pipeline.gs_unit_exclusive_configuration &&
            regs.internal.pipeline.use_gs == PipelineRegs::UseGS::No) {
            gs_setup.WriteUniformIntReg(index, regs.internal.vs.GetIntUniform(index));
        }
        break;
    }

    case PICA_REG_INDEX(vs.uniform_setup.set_value[0]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[1]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[2]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[3]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[4]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[5]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[6]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[7]): {
        const auto index = vs_setup.WriteUniformFloatReg(regs.internal.vs, value);
        if (!regs.internal.pipeline.gs_unit_exclusive_configuration &&
            regs.internal.pipeline.use_gs == PipelineRegs::UseGS::No && index) {
            gs_setup.uniforms.f[index.value()] = vs_setup.uniforms.f[index.value()];
        }
        break;
    }

    case PICA_REG_INDEX(vs.program.set_word[0]):
    case PICA_REG_INDEX(vs.program.set_word[1]):
    case PICA_REG_INDEX(vs.program.set_word[2]):
    case PICA_REG_INDEX(vs.program.set_word[3]):
    case PICA_REG_INDEX(vs.program.set_word[4]):
    case PICA_REG_INDEX(vs.program.set_word[5]):
    case PICA_REG_INDEX(vs.program.set_word[6]):
    case PICA_REG_INDEX(vs.program.set_word[7]): {
        u32& offset = regs.internal.vs.program.offset;
        if (offset >= 512) {
            LOG_ERROR(HW_GPU, "Invalid VS program offset {}", offset);
        } else {
            vs_setup.UpdateProgramCode(offset, value);
            if (!regs.internal.pipeline.gs_unit_exclusive_configuration &&
                regs.internal.pipeline.use_gs == PipelineRegs::UseGS::No) {
                gs_setup.UpdateProgramCode(offset, value);
            }
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[1]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[2]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[3]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[4]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[5]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[6]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[7]): {
        u32& offset = regs.internal.vs.swizzle_patterns.offset;
        if (offset >= vs_setup.GetSwizzleData().size()) {
            LOG_ERROR(HW_GPU, "Invalid VS swizzle pattern offset {}", offset);
        } else {
            vs_setup.UpdateSwizzleData(offset, value);
            if (!regs.internal.pipeline.gs_unit_exclusive_configuration &&
                regs.internal.pipeline.use_gs == PipelineRegs::UseGS::No) {
                gs_setup.UpdateSwizzleData(offset, value);
            }
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(lighting.lut_data[0]):
    case PICA_REG_INDEX(lighting.lut_data[1]):
    case PICA_REG_INDEX(lighting.lut_data[2]):
    case PICA_REG_INDEX(lighting.lut_data[3]):
    case PICA_REG_INDEX(lighting.lut_data[4]):
    case PICA_REG_INDEX(lighting.lut_data[5]):
    case PICA_REG_INDEX(lighting.lut_data[6]):
    case PICA_REG_INDEX(lighting.lut_data[7]): {
        auto& lut_config = regs.internal.lighting.lut_config;
        ASSERT_MSG(lut_config.index < 256, "lut_config.index exceeded maximum value of 255!");

        const u32 prev = std::exchange(lighting.luts[lut_config.type][lut_config.index].raw, value);
        lighting.lut_dirty |= (prev != value) << lut_config.type;
        lut_config.index.Assign(lut_config.index + 1);
        break;
    }

    case PICA_REG_INDEX(texturing.fog_lut_data[0]):
    case PICA_REG_INDEX(texturing.fog_lut_data[1]):
    case PICA_REG_INDEX(texturing.fog_lut_data[2]):
    case PICA_REG_INDEX(texturing.fog_lut_data[3]):
    case PICA_REG_INDEX(texturing.fog_lut_data[4]):
    case PICA_REG_INDEX(texturing.fog_lut_data[5]):
    case PICA_REG_INDEX(texturing.fog_lut_data[6]):
    case PICA_REG_INDEX(texturing.fog_lut_data[7]): {
        const u32 prev =
            std::exchange(fog.lut[regs.internal.texturing.fog_lut_offset % 128].raw, value);
        fog.lut_dirty |= prev != value;
        regs.internal.texturing.fog_lut_offset.Assign(regs.internal.texturing.fog_lut_offset + 1);
        break;
    }

    case PICA_REG_INDEX(texturing.proctex_lut_data[0]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[1]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[2]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[3]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[4]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[5]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[6]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[7]): {
        auto& index = regs.internal.texturing.proctex_lut_config.index;
        const auto lut_table = regs.internal.texturing.proctex_lut_config.ref_table.Value();

        const auto sync_lut = [&](auto& proctex_table) {
            const u32 prev = std::exchange(proctex_table[index % proctex_table.size()].raw, value);
            proctex.table_dirty |= (prev != value) << u32(lut_table);
        };

        switch (lut_table) {
        case TexturingRegs::ProcTexLutTable::Noise:
            sync_lut(proctex.noise_table);
            break;
        case TexturingRegs::ProcTexLutTable::ColorMap:
            sync_lut(proctex.color_map_table);
            break;
        case TexturingRegs::ProcTexLutTable::AlphaMap:
            sync_lut(proctex.alpha_map_table);
            break;
        case TexturingRegs::ProcTexLutTable::Color:
            sync_lut(proctex.color_table);
            break;
        case TexturingRegs::ProcTexLutTable::ColorDiff:
            sync_lut(proctex.color_diff_table);
            break;
        }
        index.Assign(index + 1);
        break;
    }
    default:
        break;
    }

    dirty_regs.Set(id);

    if (debug_context) {
        debug_context->OnEvent(DebugContext::Event::PicaCommandProcessed, &id);
    }
}

void PicaCore::SubmitImmediate(u32 value) {
    // Push to word to the queue. This returns true when a full attribute is formed.
    if (!immediate.queue.Push(value)) {
        return;
    }

    constexpr std::size_t IMMEDIATE_MODE_INDEX = 0xF;

    auto& setup = regs.internal.pipeline.vs_default_attributes_setup;
    if (setup.index > IMMEDIATE_MODE_INDEX) {
        LOG_ERROR(HW_GPU, "Invalid VS default attribute index {}", setup.index);
        return;
    }

    // Retrieve the attribute and place it in the default attribute buffer.
    const auto attribute = immediate.queue.Get();
    if (setup.index < IMMEDIATE_MODE_INDEX) {
        input_default_attributes[setup.index] = attribute;
        setup.index++;
        return;
    }

    // When index is 0xF the attribute is used for immediate mode drawing.
    immediate.input_vertex[immediate.current_attribute] = attribute;
    if (immediate.current_attribute < regs.internal.pipeline.max_input_attrib_index) {
        immediate.current_attribute++;
        return;
    }

    // We formed a vertex, flush.
    DrawImmediate();
}

void PicaCore::DrawImmediate() {
    // Compile the vertex shader.
    shader_engine->SetupBatch(vs_setup, regs.internal.vs.main_offset);

    // Track vertex in the debug recorder.
    if (debug_context) {
        debug_context->OnEvent(DebugContext::Event::VertexShaderInvocation,
                               std::addressof(immediate.input_vertex));
    }

    ShaderUnit shader_unit;
    AttributeBuffer output{};

    // Invoke the vertex shader for the vertex.
    shader_unit.LoadInput(regs.internal.vs, immediate.input_vertex);
    shader_engine->Run(vs_setup, shader_unit);
    shader_unit.WriteOutput(regs.internal.vs, output);

    // Reconfigure geometry pipeline if needed.
    if (immediate.reset_geometry_pipeline) {
        geometry_pipeline.Reconfigure();
        immediate.reset_geometry_pipeline = false;
    }

    // Send to geometry pipeline.
    ASSERT(!geometry_pipeline.NeedIndexInput());
    geometry_pipeline.Setup(shader_engine.get());
    geometry_pipeline.SubmitVertex(output);

    // Flush the immediate triangle.
    rasterizer->DrawTriangles();
    immediate.current_attribute = 0;

    if (debug_context) {
        debug_context->OnEvent(DebugContext::Event::FinishedPrimitiveBatch, nullptr);
    }
}

void PicaCore::DrawArrays(bool is_indexed) {
    MICROPROFILE_SCOPE(GPU_Drawing);
#ifdef __SWITCH__
    const bool switch_trace_draw_profile = SwitchPicaReserveLog();
    const auto switch_draw_profile_start =
        switch_trace_draw_profile ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    const u32 switch_vertices = regs.internal.pipeline.num_vertices;
    const u32 switch_vertex_offset = regs.internal.pipeline.vertex_offset;
    const bool switch_use_hw_shader = Settings::values.use_hw_shader.GetValue();
    const bool switch_primitive_empty = primitive_assembler.IsEmpty();
    const auto switch_assembler_topology = primitive_assembler.GetTopology();
    const auto switch_pipeline_topology = regs.internal.pipeline.triangle_topology.Value();
    const auto switch_use_gs = regs.internal.pipeline.use_gs.Value();
#endif

    // Track vertex in the debug recorder.
    if (debug_context) {
        debug_context->OnEvent(DebugContext::Event::IncomingPrimitiveBatch, nullptr);
    }

    const bool accelerate_draw = [this] {
        // Geometry shaders cannot be accelerated due to register preservation.
        if (regs.internal.pipeline.use_gs == PipelineRegs::UseGS::Yes) {
            return false;
        }

        // TODO (wwylele): for Strip/Fan topology, if the primitive assember is not restarted
        // after this draw call, the buffered vertex from this draw should "leak" to the next
        // draw, in which case we should buffer the vertex into the software primitive assember,
        // or disable accelerate draw completely. However, there is not game found yet that does
        // this, so this is left unimplemented for now. Revisit this when an issue is found in
        // games.

        bool accelerate_draw = Settings::values.use_hw_shader && primitive_assembler.IsEmpty();
        const auto topology = primitive_assembler.GetTopology();
        if (topology == PipelineRegs::TriangleTopology::Shader ||
            topology == PipelineRegs::TriangleTopology::List) {
            accelerate_draw = accelerate_draw && (regs.internal.pipeline.num_vertices % 3) == 0;
        }
        return accelerate_draw;
    }();

    // Add vertices to the delay generator.
    delay_generator.AddVertices(regs.internal.pipeline.num_vertices,
                                regs.internal.pipeline.triangle_topology);

    // Attempt to use hardware vertex shaders if possible.
    bool accelerated = false;
#ifdef __SWITCH__
    long long switch_accel_ms = 0;
    long long switch_load_ms = 0;
    long long switch_raster_ms = 0;
#endif
    if (accelerate_draw) {
#ifdef __SWITCH__
        const auto switch_accel_start =
            switch_trace_draw_profile ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
#endif
        accelerated = rasterizer->AccelerateDrawBatch(is_indexed);
#ifdef __SWITCH__
        switch_accel_ms =
            switch_trace_draw_profile ? SwitchPicaElapsedMs(switch_accel_start) : 0;
#endif
        if (accelerated) {
#ifdef __SWITCH__
            const auto switch_total_ms =
                switch_trace_draw_profile ? SwitchPicaElapsedMs(switch_draw_profile_start) : 0;
            if (switch_trace_draw_profile && SwitchPicaShouldLogSlow(switch_total_ms)) {
                Azahar::Switch::AppendLogFormat(
                    nullptr,
                    "android-flow stage=pica.draw.profile path=accelerated total-ms=%lld "
                    "accel-ms=%lld indexed=%u vertices=%u vertex-offset=%u use-hw=%u "
                    "prim-empty=%u asm-topology=%u pipe-topology=%u use-gs=%u",
                    switch_total_ms, switch_accel_ms, is_indexed ? 1U : 0U, switch_vertices,
                    switch_vertex_offset, switch_use_hw_shader ? 1U : 0U,
                    switch_primitive_empty ? 1U : 0U,
                    static_cast<u32>(switch_assembler_topology),
                    static_cast<u32>(switch_pipeline_topology), static_cast<u32>(switch_use_gs));
            }
#endif
            return;
        }
    }

    // We cannot accelerate the draw, so load and execute the vertex shader for each vertex.
#ifdef __SWITCH__
    const auto switch_load_start =
        switch_trace_draw_profile ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
#endif
    LoadVertices(is_indexed);
#ifdef __SWITCH__
    switch_load_ms = switch_trace_draw_profile ? SwitchPicaElapsedMs(switch_load_start) : 0;
#endif

    // Draw emitted triangles.
#ifdef __SWITCH__
    const auto switch_raster_start =
        switch_trace_draw_profile ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
#endif
    rasterizer->DrawTriangles();
#ifdef __SWITCH__
    switch_raster_ms = switch_trace_draw_profile ? SwitchPicaElapsedMs(switch_raster_start) : 0;
    const auto switch_total_ms =
        switch_trace_draw_profile ? SwitchPicaElapsedMs(switch_draw_profile_start) : 0;
    if (switch_trace_draw_profile && SwitchPicaShouldLogSlow(switch_total_ms)) {
        Azahar::Switch::AppendLogFormat(
            nullptr,
            "android-flow stage=pica.draw.profile path=cpu-fallback total-ms=%lld accel-ms=%lld "
            "load-ms=%lld raster-ms=%lld accel-candidate=%u indexed=%u vertices=%u "
            "vertex-offset=%u use-hw=%u prim-empty=%u asm-topology=%u pipe-topology=%u "
            "use-gs=%u",
            switch_total_ms, switch_accel_ms, switch_load_ms, switch_raster_ms,
            accelerate_draw ? 1U : 0U, is_indexed ? 1U : 0U, switch_vertices,
            switch_vertex_offset, switch_use_hw_shader ? 1U : 0U, switch_primitive_empty ? 1U : 0U,
            static_cast<u32>(switch_assembler_topology),
            static_cast<u32>(switch_pipeline_topology), static_cast<u32>(switch_use_gs));
    }
#endif

    if (debug_context) {
        debug_context->OnEvent(DebugContext::Event::FinishedPrimitiveBatch, nullptr);
    }
}

void PicaCore::LoadVertices(bool is_indexed) {
    // Read and validate vertex information from the loaders
    const auto& pipeline = regs.internal.pipeline;
    const PAddr base_address = pipeline.vertex_attributes.GetPhysicalBaseAddress();
    const auto loader = VertexLoader(memory, pipeline);
    regs.internal.rasterizer.ValidateSemantics();

    // Locate index buffer.
    const auto& index_info = pipeline.index_array;
    const u8* index_address_8 = memory.GetPhysicalPointer(base_address + index_info.offset);
    const u16* index_address_16 = reinterpret_cast<const u16*>(index_address_8);
    const bool index_u16 = index_info.format != 0;

    // Simple circular-replacement vertex cache
    const std::size_t VERTEX_CACHE_SIZE = 64;
    std::array<bool, VERTEX_CACHE_SIZE> vertex_cache_valid{};
    std::array<u16, VERTEX_CACHE_SIZE> vertex_cache_ids;
    std::array<AttributeBuffer, VERTEX_CACHE_SIZE> vertex_cache;
    u32 vertex_cache_pos = 0;

    // Compile the vertex shader for this batch.
    ShaderUnit shader_unit;
    AttributeBuffer vs_output;
    shader_engine->SetupBatch(vs_setup, regs.internal.vs.main_offset);

    // Setup geometry pipeline in case we are using a geometry shader.
    geometry_pipeline.Reconfigure();
    geometry_pipeline.Setup(shader_engine.get());
    ASSERT(!geometry_pipeline.NeedIndexInput() || is_indexed);

    for (u32 index = 0; index < pipeline.num_vertices; ++index) {
        // Indexed rendering doesn't use the start offset
        const u32 vertex = is_indexed
                               ? (index_u16 ? index_address_16[index] : index_address_8[index])
                               : (index + pipeline.vertex_offset);

        bool vertex_cache_hit = false;
        if (is_indexed) {
            if (geometry_pipeline.NeedIndexInput()) {
                geometry_pipeline.SubmitIndex(vertex);
                continue;
            }

            for (u32 i = 0; i < VERTEX_CACHE_SIZE; ++i) {
                if (vertex_cache_valid[i] && vertex == vertex_cache_ids[i]) {
                    vs_output = vertex_cache[i];
                    vertex_cache_hit = true;
                    break;
                }
            }
        }

        if (!vertex_cache_hit) {
            // Initialize data for the current vertex
            AttributeBuffer input;
            loader.LoadVertex(base_address, index, vertex, input, input_default_attributes);

            // Record vertex processing to the debugger.
            if (debug_context) {
                debug_context->OnEvent(DebugContext::Event::VertexShaderInvocation,
                                       std::addressof(input));
            }

            // Invoke the vertex shader for this vertex.
            shader_unit.LoadInput(regs.internal.vs, input);
            shader_engine->Run(vs_setup, shader_unit);
            shader_unit.WriteOutput(regs.internal.vs, vs_output);

            // Cache the vertex when doing indexed rendering.
            if (is_indexed) {
                vertex_cache[vertex_cache_pos] = vs_output;
                vertex_cache_valid[vertex_cache_pos] = true;
                vertex_cache_ids[vertex_cache_pos] = vertex;
                vertex_cache_pos = (vertex_cache_pos + 1) % VERTEX_CACHE_SIZE;
            }
        }

        // Send to geometry pipeline
        geometry_pipeline.SubmitVertex(vs_output);
    }
}

PicaCore::RenderPropertiesGuess PicaCore::GuessCmdRenderProperties(PAddr list, u32 size) {
    // Initialize command list tracking.
    const u8* head = memory.GetPhysicalPointer(list);
    cmd_list.Reset(list, head, size);

    constexpr size_t max_iterations = 0x100;

    RenderPropertiesGuess find_info{};

    find_info.vp_height = regs.internal.rasterizer.viewport_size_y.Value();
    find_info.paddr = regs.internal.framebuffer.framebuffer.color_buffer_address.Value() * 8;

    auto process_write = [this, &find_info](u32 cmd_id, u32 value) {
        switch (cmd_id) {
        case PICA_REG_INDEX(rasterizer.viewport_size_y):
            find_info.vp_height = value;
            find_info.vp_heigh_found = true;
            break;
        case PICA_REG_INDEX(framebuffer.framebuffer.color_buffer_address):
            find_info.paddr = value * 8;
            find_info.paddr_found = true;
            break;
        [[unlikely]] case PICA_REG_INDEX(pipeline.command_buffer.trigger[0]):
        [[unlikely]] case PICA_REG_INDEX(pipeline.command_buffer.trigger[1]): {
            const u32 index =
                static_cast<u32>(cmd_id - PICA_REG_INDEX(pipeline.command_buffer.trigger[0]));
            const PAddr addr = regs.internal.pipeline.command_buffer.GetPhysicalAddress(index);
            const u32 size = regs.internal.pipeline.command_buffer.GetSize(index);
            const u8* head = memory.GetPhysicalPointer(addr);
            cmd_list.Reset(addr, head, size);
            break;
        }
        default:
            break;
        }
        return find_info.vp_heigh_found && find_info.paddr_found;
    };

    size_t iterations = 0;
    while (cmd_list.current_index < cmd_list.length && iterations < max_iterations) {
        // Align read pointer to 8 bytes
        if (cmd_list.current_index % 2 != 0) {
            cmd_list.current_index++;
        }

        // Read the header and the value to write.
        const u32 value = cmd_list.head[cmd_list.current_index++];
        const CommandHeader header{cmd_list.head[cmd_list.current_index++]};

        // Write to the requested PICA register.
        if (process_write(header.cmd_id, value))
            break;

        // Write any extra paramters as well.
        for (u32 i = 0; i < header.extra_data_length; ++i) {
            const u32 cmd = header.cmd_id + (header.group_commands ? i + 1 : 0);
            const u32 extra_value = cmd_list.head[cmd_list.current_index++];
            if (process_write(cmd, extra_value))
                break;
        }

        iterations++;
    }

    return find_info;
}

template <class Archive>
void PicaCore::CommandList::serialize(Archive& ar, const u32 file_version) {
    ar & addr;
    ar & length;
    ar & current_index;
    if (Archive::is_loading::value) {
        const u8* ptr = Core::System::GetInstance().Memory().GetPhysicalPointer(addr);
        head = reinterpret_cast<const u32*>(ptr);
    }
}

SERIALIZE_IMPL(PicaCore::CommandList)

} // namespace Pica
