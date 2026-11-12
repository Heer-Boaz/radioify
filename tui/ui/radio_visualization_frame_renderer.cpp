#include "radio_visualization_frame_renderer.h"

#include <string>
#include <utility>

#include "playback/video/gpu/gpu_runtime.h"
#include "radio_visualization_scene.h"

namespace tui_radio_visualization {

FrameRenderer::FrameRenderer(GpuRuntime& runtime)
    : runtime_(runtime), shader_(runtime.mutex()) {}

const AsciiArt* FrameRenderer::render(const Levels* levels, uint64_t generation,
                                      int width, int height) {
  if (width <= 0 || height <= 0) return nullptr;
  const bool sceneChanged = !hasSceneIdentity_ ||
      previousCircuitVisible_ != (levels != nullptr) ||
      previousGeneration_ != generation;
  if (sceneChanged || layoutWidth_ != width || layoutHeight_ != height) {
    art_ = {};
    gpuArt_ = {};
    haveFrame_ = false;
    cpu_.resetHistory();
    shader_.ResetSessionState();
    hasSceneIdentity_ = true;
    previousCircuitVisible_ = levels != nullptr;
    previousGeneration_ = generation;
    layoutWidth_ = width;
    layoutHeight_ = height;
  }

  drawRadio(pixels_, levels);
  if (!shaderAttempted_) {
    shaderAttempted_ = true;
    if (auto* device = runtime_.device()) {
      shaderInitialized_ = shader_.InitializeWithDevice(
          device, runtime_.asciiRenderer().context());
    }
  }
  std::string error;
  if (shaderInitialized_) {
    gpuArt_.width = width;
    gpuArt_.height = height;
    if (shader_.Render(pixels_.data(), kWidth, kHeight, gpuArt_, &error)) {
      std::swap(art_, gpuArt_);
      haveFrame_ = true;
    } else if (!error.empty()) {
      // A pending readback is normal. Switch backend only for an actual error.
      shaderInitialized_ = false;
      cpu_.resetHistory();
      haveFrame_ = false;
    }
  }
  if (!shaderInitialized_) {
    haveFrame_ = cpu_.renderRgbaExact(pixels_.data(), kWidth, kHeight,
                                      width, height, art_, true);
  }
  return haveFrame_ ? &art_ : nullptr;
}

}  // namespace tui_radio_visualization
