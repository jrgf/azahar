// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.
//
// PICA → deko3d enum/format translation tables. Mirrors pica_to_vk.h so the
// HW shader path can stay structurally identical to the Vulkan renderer.

#pragma once

#include <array>

#include <deko3d.hpp>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/vector_math.h"
#include "video_core/pica/regs_internal.h"

namespace PicaToDeko {

using TextureFilter = Pica::TexturingRegs::TextureConfig::TextureFilter;

inline DkFilter TextureFilterMode(TextureFilter mode) {
    switch (mode) {
    case TextureFilter::Linear:
        return DkFilter_Linear;
    case TextureFilter::Nearest:
        return DkFilter_Nearest;
    default:
        UNIMPLEMENTED_MSG("Unknown texture filtering mode {}", mode);
    }
    return DkFilter_Linear;
}

inline DkMipFilter TextureMipFilterMode(TextureFilter mip) {
    switch (mip) {
    case TextureFilter::Linear:
        return DkMipFilter_Linear;
    case TextureFilter::Nearest:
        return DkMipFilter_Nearest;
    default:
        UNIMPLEMENTED_MSG("Unknown texture mipmap filtering mode {}", mip);
    }
    return DkMipFilter_Linear;
}

inline DkWrapMode WrapMode(Pica::TexturingRegs::TextureConfig::WrapMode mode) {
    static constexpr std::array<DkWrapMode, 8> wrap_mode_table{{
        DkWrapMode_ClampToEdge,
        DkWrapMode_ClampToBorder,
        DkWrapMode_Repeat,
        DkWrapMode_MirroredRepeat,
        // TODO(wwylele): ClampToEdge2 / ClampToBorder2 share the same mapping as their
        // non-suffixed counterparts; see the PICA WrapMode enum for context.
        DkWrapMode_ClampToEdge,
        DkWrapMode_ClampToBorder,
        DkWrapMode_Repeat,
        DkWrapMode_Repeat,
    }};

    const auto index = static_cast<std::size_t>(mode);
    ASSERT_MSG(index < wrap_mode_table.size(), "Unknown texture wrap mode {}", index);
    return wrap_mode_table[index];
}

inline DkBlendOp BlendEquation(Pica::FramebufferRegs::BlendEquation equation) {
    static constexpr std::array<DkBlendOp, 5> blend_equation_table{{
        DkBlendOp_Add,
        DkBlendOp_Sub,
        DkBlendOp_RevSub,
        DkBlendOp_Min,
        DkBlendOp_Max,
    }};

    const auto index = static_cast<std::size_t>(equation);
    if (index >= blend_equation_table.size()) {
        LOG_CRITICAL(Render, "Unknown blend equation {}", index);
        // This return value mirrors the Vulkan path's hw-tested fallback.
        return DkBlendOp_Add;
    }
    return blend_equation_table[index];
}

inline DkBlendFactor BlendFunc(Pica::FramebufferRegs::BlendFactor factor) {
    static constexpr std::array<DkBlendFactor, 15> blend_func_table{{
        DkBlendFactor_Zero,             // BlendFactor::Zero
        DkBlendFactor_One,              // BlendFactor::One
        DkBlendFactor_SrcColor,         // BlendFactor::SourceColor
        DkBlendFactor_InvSrcColor,      // BlendFactor::OneMinusSourceColor
        DkBlendFactor_DstColor,         // BlendFactor::DestColor
        DkBlendFactor_InvDstColor,      // BlendFactor::OneMinusDestColor
        DkBlendFactor_SrcAlpha,         // BlendFactor::SourceAlpha
        DkBlendFactor_InvSrcAlpha,      // BlendFactor::OneMinusSourceAlpha
        DkBlendFactor_DstAlpha,         // BlendFactor::DestAlpha
        DkBlendFactor_InvDstAlpha,      // BlendFactor::OneMinusDestAlpha
        DkBlendFactor_ConstColor,       // BlendFactor::ConstantColor
        DkBlendFactor_InvConstColor,    // BlendFactor::OneMinusConstantColor
        DkBlendFactor_ConstAlpha,       // BlendFactor::ConstantAlpha
        DkBlendFactor_InvConstAlpha,    // BlendFactor::OneMinusConstantAlpha
        DkBlendFactor_SrcAlphaSaturate, // BlendFactor::SourceAlphaSaturate
    }};

    const auto index = static_cast<std::size_t>(factor);
    if (index >= blend_func_table.size()) {
        LOG_CRITICAL(Render, "Unknown blend factor {}", index);
        return DkBlendFactor_One;
    }
    return blend_func_table[index];
}

inline DkLogicOp LogicOp(Pica::FramebufferRegs::LogicOp op) {
    static constexpr std::array<DkLogicOp, 16> logic_op_table{{
        DkLogicOp_Clear,        // Clear
        DkLogicOp_And,          // And
        DkLogicOp_AndReverse,   // AndReverse
        DkLogicOp_Copy,         // Copy
        DkLogicOp_Set,          // Set
        DkLogicOp_CopyInverted, // CopyInverted
        DkLogicOp_NoOp,         // NoOp
        DkLogicOp_Invert,       // Invert
        DkLogicOp_Nand,         // Nand
        DkLogicOp_Or,           // Or
        DkLogicOp_Nor,          // Nor
        DkLogicOp_Xor,          // Xor
        DkLogicOp_Equivalent,   // Equiv
        DkLogicOp_AndInverted,  // AndInverted
        DkLogicOp_OrReverse,    // OrReverse
        DkLogicOp_OrInverted,   // OrInverted
    }};

    const auto index = static_cast<std::size_t>(op);
    ASSERT_MSG(index < logic_op_table.size(), "Unknown logic op {}", index);
    return logic_op_table[index];
}

inline DkCompareOp CompareFunc(Pica::FramebufferRegs::CompareFunc func) {
    static constexpr std::array<DkCompareOp, 8> compare_func_table{{
        DkCompareOp_Never,    // CompareFunc::Never
        DkCompareOp_Always,   // CompareFunc::Always
        DkCompareOp_Equal,    // CompareFunc::Equal
        DkCompareOp_NotEqual, // CompareFunc::NotEqual
        DkCompareOp_Less,     // CompareFunc::LessThan
        DkCompareOp_Lequal,   // CompareFunc::LessThanOrEqual
        DkCompareOp_Greater,  // CompareFunc::GreaterThan
        DkCompareOp_Gequal,   // CompareFunc::GreaterThanOrEqual
    }};

    const auto index = static_cast<std::size_t>(func);
    ASSERT_MSG(index < compare_func_table.size(), "Unknown compare function {}", index);
    return compare_func_table[index];
}

inline DkStencilOp StencilOp(Pica::FramebufferRegs::StencilAction action) {
    static constexpr std::array<DkStencilOp, 8> stencil_op_table{{
        DkStencilOp_Keep,     // StencilAction::Keep
        DkStencilOp_Zero,     // StencilAction::Zero
        DkStencilOp_Replace,  // StencilAction::Replace
        DkStencilOp_Incr,     // StencilAction::Increment
        DkStencilOp_Decr,     // StencilAction::Decrement
        DkStencilOp_Invert,   // StencilAction::Invert
        DkStencilOp_IncrWrap, // StencilAction::IncrementWrap
        DkStencilOp_DecrWrap, // StencilAction::DecrementWrap
    }};

    const auto index = static_cast<std::size_t>(action);
    ASSERT_MSG(index < stencil_op_table.size(), "Unknown stencil op {}", index);
    return stencil_op_table[index];
}

inline DkPrimitive PrimitiveTopology(Pica::PipelineRegs::TriangleTopology topology) {
    switch (topology) {
    case Pica::PipelineRegs::TriangleTopology::Fan:
        return DkPrimitive_TriangleFan;
    case Pica::PipelineRegs::TriangleTopology::List:
    case Pica::PipelineRegs::TriangleTopology::Shader:
        return DkPrimitive_Triangles;
    case Pica::PipelineRegs::TriangleTopology::Strip:
        return DkPrimitive_TriangleStrip;
    default:
        UNREACHABLE_MSG("Unknown triangle topology {}", topology);
    }
    return DkPrimitive_Triangles;
}

inline DkFace CullMode(Pica::RasterizerRegs::CullMode mode, bool flip_viewport) {
    switch (mode) {
    case Pica::RasterizerRegs::CullMode::KeepAll:
        return DkFace_None;
    case Pica::RasterizerRegs::CullMode::KeepClockWise:
    case Pica::RasterizerRegs::CullMode::KeepCounterClockWise:
        return flip_viewport ? DkFace_Front : DkFace_Back;
    default:
        UNREACHABLE_MSG("Unknown cull mode {}", mode);
    }
    return DkFace_None;
}

inline DkFrontFace FrontFace(Pica::RasterizerRegs::CullMode mode) {
    switch (mode) {
    case Pica::RasterizerRegs::CullMode::KeepAll:
    case Pica::RasterizerRegs::CullMode::KeepClockWise:
        return DkFrontFace_CCW;
    case Pica::RasterizerRegs::CullMode::KeepCounterClockWise:
        return DkFrontFace_CW;
    default:
        UNREACHABLE_MSG("Unknown cull mode {}", mode);
    }
    return DkFrontFace_CW;
}

inline Common::Vec4f ColorRGBA8(const u32 color) {
    const auto rgba =
        Common::Vec4u{color >> 0 & 0xFF, color >> 8 & 0xFF, color >> 16 & 0xFF, color >> 24 & 0xFF};
    return rgba / 255.0f;
}

} // namespace PicaToDeko
