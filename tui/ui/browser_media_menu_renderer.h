#pragma once

#include "tui/consolescreen.h"
#include "tui/ui/browser_media_menu.h"
#include "tui/ui/popup_menu_renderer.h"

namespace tui_browser_media_menu {

void draw(ConsoleScreen& screen, Model& model,
          const tui_popup_menu::Bounds& bounds,
          const tui_popup_menu::Styles& styles);

}  // namespace tui_browser_media_menu
