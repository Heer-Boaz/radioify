#pragma once

#include <utility>

#include "asciiart.h"
#include "gpu_text_grid.h"
#include "videowindow.h"

namespace playback_framebuffer_presenter {

void buildPlaybackMiniPlayerTuiFrame(const WindowUiState& ui, int pixelWidth,
                                     int pixelHeight,
                                     GpuTextGridFrame& outFrame);

std::pair<int, int> computePlaybackMiniPlayerAsciiSize(int pixelWidth,
                                                       int pixelHeight,
                                                       int srcWidth,
                                                       int srcHeight);

void buildPlaybackMiniPlayerAsciiFrame(const AsciiArt& art,
                                       const WindowUiState& ui,
                                       GpuTextGridFrame& outFrame);

}  // namespace playback_framebuffer_presenter
