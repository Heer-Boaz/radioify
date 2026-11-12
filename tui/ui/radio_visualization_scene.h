#pragma once

#include <cstdint>
#include <vector>

#include "radio_visualization_model.h"

namespace tui_radio_visualization {

constexpr int kWidth = 720;
constexpr int kHeight = 440;
void drawRadio(std::vector<uint8_t>& pixels, const Levels* levels);

}  // namespace tui_radio_visualization
