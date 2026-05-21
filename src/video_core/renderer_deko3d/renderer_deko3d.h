// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version.
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "video_core/rasterizer_accelerated.h"
#include "video_core/renderer_base.h"

namespace Core {
class System;
}

namespace Pica {
class PicaCore;
}

namespace Deko3D {

struct PresentVertex {
    std::array<float, 4> position{};
    std::array<float, 4> color{};
    std::array<float, 2> tex_coord0{};
};

struct PresentTextureInfo {
    PAddr address = 0;
    u32 width = 0;
    u32 height = 0;
    u32 format = 0;
    u32 type = 0;
    bool enabled = false;
};

struct PresentBatch {
    std::vector<PresentVertex> vertices;
    PresentTextureInfo texture;
};

struct DisplayTransferRecord {
    PAddr input_address = 0;
    PAddr output_address = 0;
    u32 input_width = 0;
    u32 input_height = 0;
    u32 output_width = 0;
    u32 output_height = 0;
    u32 input_format = 0;
    u32 output_format = 0;
    u32 flags = 0;
    u32 scaling = 0;
};

class RasterizerDeko3D final : public VideoCore::RasterizerAccelerated {
public:
    explicit RasterizerDeko3D(Memory::MemorySystem& memory, Pica::PicaCore& pica);
    ~RasterizerDeko3D() override;

    void DrawTriangles() override;
    void FlushAll() override {}
    void FlushRegion(PAddr addr, u32 size) override {}
    void InvalidateRegion(PAddr addr, u32 size) override {}
    void FlushAndInvalidateRegion(PAddr addr, u32 size) override {}
    void ClearAll(bool flush) override;
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateDrawBatch(bool is_indexed) override;
    std::vector<DisplayTransferRecord> ConsumeDisplayTransfers();
    std::vector<PresentBatch> ConsumePresentBatches();

private:
    u32 draw_batch_log_count = 0;
    u32 fallback_draw_log_count = 0;
    u32 render_target_log_count = 0;
    u32 texture_sample_log_count = 0;
    u32 display_transfer_log_count = 0;
    std::vector<DisplayTransferRecord> display_transfers;
    std::vector<PresentBatch> present_batches;
};

class RendererDeko3D final : public VideoCore::RendererBase {
public:
    explicit RendererDeko3D(Core::System& system, Pica::PicaCore& pica,
                            Frontend::EmuWindow& window);
    ~RendererDeko3D() override;

    [[nodiscard]] VideoCore::RasterizerInterface* Rasterizer() override {
        return &rasterizer;
    }

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override {}

private:
    struct Context;

    bool EnsureContext();

    Memory::MemorySystem& memory;
    Pica::PicaCore& pica;
    RasterizerDeko3D rasterizer;
    std::unique_ptr<Context> context;
    u32 frame_count = 0;
    bool init_failed = false;
};

} // namespace Deko3D
