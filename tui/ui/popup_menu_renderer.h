#pragma once

#include "consolescreen.h"
#include "popup_menu.h"

namespace tui_popup_menu {

struct Styles {
  Style normal;
  Style border;
  Style selected;
};

void draw(ConsoleScreen& screen, Model& model, const Bounds& bounds,
          const Styles& styles);

}  // namespace tui_popup_menu
