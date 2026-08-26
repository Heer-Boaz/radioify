#pragma once

#include "command_palette.h"
#include "consolescreen.h"

namespace tui_command_palette {

struct Styles {
  Style normal;
  Style border;
  Style dim;
  Style selected;
};

void draw(ConsoleScreen& screen, Model& model,
          const std::vector<Command>& commands, const Bounds& bounds,
          const Styles& styles);

}  // namespace tui_command_palette
