#include "tui/ui/browser_media_menu_renderer.h"

namespace tui_browser_media_menu {

void draw(ConsoleScreen& screen, Model& model,
          const tui_popup_menu::Bounds& bounds,
          const tui_popup_menu::Styles& styles) {
  tui_popup_menu::draw(screen, model.popup_, bounds, styles);
}

}  // namespace tui_browser_media_menu
