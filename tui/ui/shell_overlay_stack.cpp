#include "shell_overlay_stack.h"

#include <utility>

namespace shell_overlay_stack {

Layer Model::activeLayer() const {
  if (mediaMenu_.active()) {
    return Layer::MediaMenu;
  }
  if (commandPalette_.active()) {
    return Layer::CommandPalette;
  }
  return Layer::None;
}

bool Model::openMediaMenu(
    BrowserEntry entry,
    std::vector<playback_media_actions::Item> items,
    tui_popup_menu::Anchor anchor) {
  const bool paletteDismissed = commandPalette_.dismiss();
  const bool menuWasActive = mediaMenu_.active();
  const bool menuOpened = mediaMenu_.open(
      std::move(entry), std::move(items), anchor);
  return paletteDismissed || menuWasActive || menuOpened;
}

bool Model::toggleCommandPalette() {
  if (commandPalette_.active()) {
    return commandPalette_.dismiss();
  }
  mediaMenu_.dismiss();
  commandPalette_.open();
  return true;
}

bool Model::dismiss() {
  const bool mediaMenuDismissed = mediaMenu_.dismiss();
  const bool paletteDismissed = commandPalette_.dismiss();
  return mediaMenuDismissed || paletteDismissed;
}

Interaction Model::handle(
    const InputEvent& event, const Bounds& bounds,
    const shell_command_catalog::Catalog& catalog) {
  Interaction result;
  switch (activeLayer()) {
    case Layer::MediaMenu: {
      tui_popup_menu::Bounds popupBounds;
      popupBounds.width = bounds.width;
      popupBounds.height = bounds.height;
      popupBounds.topInset = bounds.topInset;
      tui_browser_media_menu::Interaction interaction =
          mediaMenu_.handle(event, popupBounds);
      result.consumed = interaction.consumed;
      result.changed = interaction.changed;
      result.mediaCommand = std::move(interaction.command);
      break;
    }
    case Layer::CommandPalette: {
      tui_command_palette::Bounds paletteBounds;
      paletteBounds.width = bounds.width;
      paletteBounds.height = bounds.height;
      paletteBounds.topInset = bounds.topInset;
      const tui_command_palette::Interaction interaction =
          commandPalette_.handle(event, catalog.commands(), paletteBounds);
      result.consumed = interaction.consumed;
      result.changed = interaction.changed;
      if (interaction.activatedCommand) {
        if (const shell_command_catalog::Intent* intent =
                catalog.intentAt(*interaction.activatedCommand)) {
          result.paletteIntent = *intent;
        }
      }
      break;
    }
    case Layer::None:
      break;
  }
  return result;
}

}  // namespace shell_overlay_stack
