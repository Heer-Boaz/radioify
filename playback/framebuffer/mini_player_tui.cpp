#include "mini_player_tui.h"

#include <algorithm>

namespace playback_framebuffer_presenter {
namespace {

uint32_t color(GpuTextGridColor value) {
  return gpuTextGridColorRgb(value);
}

GpuTextGridCell makeCell(char ch, GpuTextGridColor fg, GpuTextGridColor bg) {
  return GpuTextGridCell{playback_gpu_text_grid::glyphIndex(
                             static_cast<unsigned char>(ch)),
                         color(fg), color(bg), 0};
}

uint32_t color(const Color& value) {
  return gpuTextGridRgb(value.r, value.g, value.b);
}

GpuTextGridCell makeScreenCell(const ScreenCell& cell) {
  const uint32_t fg = color(cell.fg);
  const uint32_t bg = color(cell.bg);
  if (cell.continuation) {
    return GpuTextGridCell{playback_gpu_text_grid::glyphIndex(' '), fg, bg, 0};
  }

  return GpuTextGridCell{playback_gpu_text_grid::glyphIndex(
                             static_cast<uint32_t>(cell.ch)),
                         fg, bg, 0};
}

}  // namespace

void buildGpuTextGridFrameFromScreenCells(const std::vector<ScreenCell>& cells,
                                          int cols, int rows,
                                          GpuTextGridFrame& outFrame) {
  cols = std::max(1, cols);
  rows = std::max(1, rows);
  outFrame.cols = cols;
  outFrame.rows = rows;

  const GpuTextGridCell background =
      makeCell(' ', GpuTextGridColor::Text, GpuTextGridColor::Background);
  const size_t cellCount =
      static_cast<size_t>(cols) * static_cast<size_t>(rows);
  if (outFrame.cells.size() != cellCount) {
    outFrame.cells.assign(cellCount, background);
  }

  const size_t available = std::min(cellCount, cells.size());
  for (size_t i = 0; i < available; ++i) {
    outFrame.cells[i] = makeScreenCell(cells[i]);
  }
  for (size_t i = available; i < cellCount; ++i) {
    outFrame.cells[i] = background;
  }
}

}  // namespace playback_framebuffer_presenter
