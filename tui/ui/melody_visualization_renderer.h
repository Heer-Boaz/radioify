#pragma once

#include "melody_visualization.h"
#include "tui/consolescreen.h"

namespace tui_melody_visualization {

struct Bounds {
  int top = 0;
  int height = 0;
  int width = 0;
};

struct Styles {
  Style normal;
  Style accent;
  Style dim;
};

void draw(ConsoleScreen &screen, const Model &model, const Bounds &bounds,
          const Styles &styles);

} // namespace tui_melody_visualization
