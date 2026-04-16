#pragma once

#include <cstdint>
#include <vector>

enum class GpuTextGridColor : uint8_t {
    Background = 0,
    Panel = 1,
    Text = 2,
    Dim = 3,
    Accent = 4,
    Fill = 5,
    Empty = 6,
    Alert = 7,
};

constexpr uint8_t gpuTextGridColorIndex(GpuTextGridColor color) {
    return static_cast<uint8_t>(color);
}

struct GpuTextGridCell {
    uint8_t ch = ' ';
    uint8_t fg = gpuTextGridColorIndex(GpuTextGridColor::Text);
    uint8_t bg = gpuTextGridColorIndex(GpuTextGridColor::Background);
    uint8_t flags = 0;
};

static_assert(sizeof(GpuTextGridCell) == 4,
              "GpuTextGridCell must match the R8G8B8A8_UINT GPU texture layout");

struct GpuTextGridFrame {
    int cols = 0;
    int rows = 0;
    std::vector<GpuTextGridCell> cells;
};
