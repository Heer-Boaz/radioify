#pragma once

#include <vector>

#include "consolescreen.h"
#include "gpu_text_grid.h"

namespace playback_framebuffer_presenter {

void buildGpuTextGridFrameFromScreenCells(const std::vector<ScreenCell>& cells,
                                          int cols, int rows,
                                          GpuTextGridFrame& outFrame);

}  // namespace playback_framebuffer_presenter
