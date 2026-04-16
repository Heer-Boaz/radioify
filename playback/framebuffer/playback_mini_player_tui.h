#pragma once

#include "gpu_text_grid.h"
#include "videowindow.h"

namespace playback_framebuffer_presenter {

void buildPlaybackMiniPlayerTuiFrame(const WindowUiState& ui, int pixelWidth,
                                     int pixelHeight,
                                     GpuTextGridFrame& outFrame);

}  // namespace playback_framebuffer_presenter
