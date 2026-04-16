#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "videowindow.h"

bool renderWindowOverlayTextToBitmap(
    const std::string& topLine,
    const std::vector<WindowUiState::ControlButton>& controlButtons,
    const std::string& fallbackControlsLine,
    const std::string& progressSuffix,
    int width,
    int height,
    std::vector<uint8_t>& outPixels,
    bool centerAlign = false,
    int padX = 1,
    int padY = 1);

