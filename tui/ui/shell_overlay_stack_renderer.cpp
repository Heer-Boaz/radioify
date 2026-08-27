#include "shell_overlay_stack_renderer.h"

namespace shell_overlay_stack {

void draw(ConsoleScreen& screen, Model& model,
          const shell_command_catalog::Catalog& catalog,
          const Bounds& bounds, const Styles& styles) {
  switch (model.activeLayer()) {
    case Layer::MediaMenu: {
      tui_popup_menu::Bounds popupBounds;
      popupBounds.width = bounds.width;
      popupBounds.height = bounds.height;
      popupBounds.topInset = bounds.topInset;
      tui_browser_media_menu::draw(screen, model.mediaMenu_, popupBounds,
                                   styles.mediaMenu);
      break;
    }
    case Layer::CommandPalette: {
      tui_command_palette::Bounds paletteBounds;
      paletteBounds.width = bounds.width;
      paletteBounds.height = bounds.height;
      paletteBounds.topInset = bounds.topInset;
      tui_command_palette::draw(screen, model.commandPalette_,
                                catalog.commands(), paletteBounds,
                                styles.commandPalette);
      break;
    }
    case Layer::None:
      break;
  }
}

}  // namespace shell_overlay_stack
