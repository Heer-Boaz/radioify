#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "radio_visualization_model.h"

namespace tui_radio_visualization {

constexpr int kWidth = 720;
constexpr int kHeight = 440;
void drawRadio(std::vector<uint8_t>& pixels, const Levels* levels);

// The raster depends only on the observed probes and their display levels;
// shader history and output size are owned separately by each renderer.
class SceneRaster {
 public:
  bool update(const Levels* levels);
  const std::vector<uint8_t>& pixels() const { return pixels_; }

 private:
  std::vector<uint8_t> pixels_;
  std::array<float, kRadioCircuitProbeCount> values_{};
  uint32_t observed_ = 0;
  bool initialized_ = false;
  bool hasLevels_ = false;
};

}  // namespace tui_radio_visualization
