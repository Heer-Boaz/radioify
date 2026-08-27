#pragma once

#include <vector>

#include "browser_action_strip.h"
#include "tui/consolescreen.h"

namespace browser_action_strip {

struct Styles {
  Style normal;
  Style active;
};

// Draws and publishes mouse-hit geometry from the same layout used by chrome
// measurement. No surface may independently reimplement action wrapping.
ActionStripLayout draw(ConsoleScreen& screen, const std::vector<Item>& items,
                       int width, int height, int top, int hoveredIndex,
                       const Styles& styles);

}  // namespace browser_action_strip
