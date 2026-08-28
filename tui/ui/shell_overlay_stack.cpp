#include "shell_overlay_stack.h"

#include <utility>

namespace shell_overlay_stack {

Layer Model::activeLayer() const {
  if (dialog_.active()) {
    return Layer::Dialog;
  }
  if (mediaMenu_.active()) {
    return Layer::MediaMenu;
  }
  if (commandPalette_.active()) {
    return Layer::CommandPalette;
  }
  return Layer::None;
}

bool Model::openMediaMenu(BrowserEntry entry,
                          std::vector<playback_media_actions::Item> items,
                          tui_popup_menu::Anchor anchor) {
  if (dialog_.active()) {
    return false;
  }
  const bool paletteDismissed = commandPalette_.dismiss();
  const bool menuWasActive = mediaMenu_.active();
  const bool menuOpened =
      mediaMenu_.open(std::move(entry), std::move(items), anchor);
  return paletteDismissed || menuWasActive || menuOpened;
}

bool Model::toggleCommandPalette() {
  if (dialog_.active()) {
    return false;
  }
  if (commandPalette_.active()) {
    return commandPalette_.dismiss();
  }
  mediaMenu_.dismiss();
  commandPalette_.open();
  return true;
}

tui_dialog::DialogId Model::openDialog(tui_dialog::Content content) {
  mediaMenu_.dismiss();
  commandPalette_.dismiss();
  return dialog_.open(std::move(content));
}

bool Model::dismissDialog(tui_dialog::DialogId expectedDialog) {
  return dialog_.dismiss(expectedDialog);
}

bool Model::dismiss() {
  const bool mediaMenuDismissed = mediaMenu_.dismiss();
  const bool paletteDismissed = commandPalette_.dismiss();
  const bool dialogDismissed = dialog_.dismiss();
  return mediaMenuDismissed || paletteDismissed || dialogDismissed;
}

Interaction Model::handle(const InputEvent& event, const Bounds& bounds,
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
    case Layer::Dialog: {
      tui_dialog::Bounds dialogBounds;
      dialogBounds.width = bounds.width;
      dialogBounds.height = bounds.height;
      dialogBounds.topInset = bounds.topInset;
      const tui_dialog::Interaction interaction =
          dialog_.handle(event, dialogBounds);
      result.consumed = interaction.consumed;
      result.changed = interaction.changed;
      result.dismissedDialog = interaction.dismissedDialog;
      result.dialogActivation = interaction.activation;
      break;
    }
    case Layer::None:
      break;
  }
  return result;
}

}  // namespace shell_overlay_stack
