#pragma once

#include "dialog.h"
#include "tui/consolescreen.h"

namespace tui_dialog {

struct Styles {
  Style background;
  Style border;
  Style title;
  Style normal;
  Style emphasis;
  Style secondary;
  Style error;
  Style button;
  Style selectedButton;
};

void draw(ConsoleScreen& screen, Model& model, const Bounds& bounds,
          const Styles& styles);

}  // namespace tui_dialog
