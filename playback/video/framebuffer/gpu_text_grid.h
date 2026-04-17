#pragma once

#include <cstdint>
#include <vector>

constexpr uint32_t gpuTextGridRgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16);
}

enum class GpuTextGridColor : uint32_t {
    Background = gpuTextGridRgb(5, 6, 7),
    Panel = gpuTextGridRgb(19, 22, 24),
    Text = gpuTextGridRgb(219, 224, 230),
    Dim = gpuTextGridRgb(112, 125, 138),
    Accent = gpuTextGridRgb(250, 176, 51),
    Fill = gpuTextGridRgb(240, 143, 31),
    Empty = gpuTextGridRgb(46, 51, 56),
    Alert = gpuTextGridRgb(235, 56, 61),
};

constexpr uint32_t gpuTextGridColorRgb(GpuTextGridColor color) {
    return static_cast<uint32_t>(color);
}

constexpr int kGpuTextGridFallbackCellPixelWidth = 9;
constexpr int kGpuTextGridFallbackCellPixelHeight = 21;
constexpr uint32_t kGpuTextGridCellFlagBraille = 1u;
constexpr uint32_t kGpuTextGridCellFlagTransparentBg = 2u;

struct GpuTextGridCell {
    uint32_t ch = ' ';
    uint32_t fg = gpuTextGridColorRgb(GpuTextGridColor::Text);
    uint32_t bg = gpuTextGridColorRgb(GpuTextGridColor::Background);
    uint32_t flags = 0;
};

static_assert(sizeof(GpuTextGridCell) == 16,
              "GpuTextGridCell must match the R32G32B32A32_UINT GPU texture layout");

struct GpuTextGridFrame {
    int cols = 0;
    int rows = 0;
    std::vector<GpuTextGridCell> cells;
};
