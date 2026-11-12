#pragma once

#include <cstdint>
#include <vector>

#include "playback/video/ascii/asciiart.h"
#include "playback/video/ascii/asciiart_gpu.h"
#include "radio_visualization_model.h"

class GpuRuntime;

namespace tui_radio_visualization {

// Owns the asynchronous shader history and the currently presented frame.
class FrameRenderer {
 public:
  explicit FrameRenderer(GpuRuntime& runtime);
  const AsciiArt* render(const Levels* levels, uint64_t generation,
                         int width, int height);

 private:
  GpuRuntime& runtime_;
  GpuAsciiRenderer shader_;
  AsciiArtRenderer cpu_;
  AsciiArt art_;
  AsciiArt gpuArt_;
  std::vector<uint8_t> pixels_;
  bool shaderInitialized_ = false;
  bool shaderAttempted_ = false;
  bool haveFrame_ = false;
  bool hasSceneIdentity_ = false;
  bool previousCircuitVisible_ = false;
  uint64_t previousGeneration_ = 0;
  int layoutWidth_ = 0;
  int layoutHeight_ = 0;
};

}  // namespace tui_radio_visualization
