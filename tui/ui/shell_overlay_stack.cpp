#include "shell_overlay_stack.h"

#include <cassert>
#include <utility>

namespace shell_overlay_stack {

DialogOwner Model::createDialogOwner() {
  DialogOwner owner{nextDialogOwnerValue_++};
  if (nextDialogOwnerValue_ == 0) {
    nextDialogOwnerValue_ = 1;
  }
  return owner;
}

std::optional<DialogLease> Model::activeDialog() const {
  const std::optional<tui_dialog::DialogId> dialog = dialog_.activeId();
  if (!dialog || !dialogOwner_) return std::nullopt;
  return DialogLease{*dialogOwner_, *dialog};
}

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

DialogOpening Model::openDialog(DialogOwner owner,
                                tui_dialog::Content content) {
  assert(owner && "dialogs require an explicit owner");
  const std::optional<DialogLease> replaced = activeDialog();
  mediaMenu_.dismiss();
  commandPalette_.dismiss();
  const tui_dialog::DialogId dialog = dialog_.open(std::move(content));
  dialogOwner_ = owner;
  return DialogOpening{DialogLease{owner, dialog}, replaced};
}

std::optional<DialogLease> Model::dismissDialog(
    tui_dialog::DialogId expectedDialog) {
  const std::optional<DialogLease> lease = activeDialog();
  if (!lease || lease->dialog != expectedDialog ||
      !dialog_.dismiss(expectedDialog)) {
    return std::nullopt;
  }
  dialogOwner_.reset();
  return lease;
}

Dismissal Model::dismiss() {
  Dismissal result;
  result.dialog = activeDialog();
  const bool mediaMenuDismissed = mediaMenu_.dismiss();
  const bool paletteDismissed = commandPalette_.dismiss();
  const bool dialogDismissed = dialog_.dismiss();
  if (dialogDismissed) dialogOwner_.reset();
  result.changed =
      mediaMenuDismissed || paletteDismissed || dialogDismissed;
  if (!dialogDismissed) result.dialog.reset();
  return result;
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
      const std::optional<DialogLease> lease = activeDialog();
      tui_dialog::Bounds dialogBounds;
      dialogBounds.width = bounds.width;
      dialogBounds.height = bounds.height;
      dialogBounds.topInset = bounds.topInset;
      const tui_dialog::Interaction interaction =
          dialog_.handle(event, dialogBounds);
      result.consumed = interaction.consumed;
      result.changed = interaction.changed;
      if (lease && interaction.dismissedDialog == lease->dialog) {
        DialogResolution resolution;
        resolution.lease = *lease;
        if (interaction.activation) {
          resolution.activatedButton = interaction.activation->button;
        }
        result.dialogResolution = resolution;
        dialogOwner_.reset();
      }
      break;
    }
    case Layer::None:
      break;
  }
  return result;
}

}  // namespace shell_overlay_stack
