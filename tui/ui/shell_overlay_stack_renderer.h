#pragma once

#include "browser_media_menu_renderer.h"
#include "command_palette_renderer.h"
#include "dialog_renderer.h"
#include "shell_overlay_stack.h"

namespace shell_overlay_stack {

struct Styles {
  tui_popup_menu::Styles mediaMenu;
  tui_command_palette::Styles commandPalette;
  tui_dialog::Styles dialog;
};

void draw(ConsoleScreen& screen, Model& model,
          const shell_command_catalog::Catalog& catalog,
          const Bounds& bounds, const Styles& styles);

}  // namespace shell_overlay_stack
