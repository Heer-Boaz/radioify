#pragma once

#include "radio_visualization_frame_renderer.h"

class ConsoleScreen;
class GpuRuntime;

namespace tui_radio_visualization {

class Renderer {
 public:
  explicit Renderer(GpuRuntime& gpu);
  void draw(ConsoleScreen& screen, int top, int height, int width,
            const Levels* levels, RadioFilterMode requestedMode,
            bool paused);
  void drawCompact(ConsoleScreen& screen, int top, int height, int width,
                   double cellWidth, double cellHeight, const Levels* levels,
                   RadioFilterMode requestedMode, bool paused);

 private:
  void drawCircuit(ConsoleScreen& screen, int top, int height, int width,
                   double cellWidth, double cellHeight, const Levels* levels,
                   bool showCircuit);
  FrameRenderer frames_;
};

}  // namespace tui_radio_visualization
