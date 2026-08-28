#include <cstdlib>
#include <iostream>
#include <vector>

#include "tui/ui/shell_overlay_stack.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << "shell_overlay_stack_tests: " << message << '\n';
  return false;
}

InputEvent keyEvent(WORD key, DWORD control = 0) {
  InputEvent event;
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
  event.key.control = control;
  return event;
}

InputEvent mouseEvent(MouseEventKind kind, int x, int y) {
  InputEvent event;
  event.type = InputEvent::Type::Mouse;
  event.mouse.kind = kind;
  event.mouse.pos.X = static_cast<SHORT>(x);
  event.mouse.pos.Y = static_cast<SHORT>(y);
  event.mouse.button = MouseButton::Left;
  event.mouse.buttons =
      kind == MouseEventKind::Press ? MouseButtons::Left : MouseButtons::None;
  return event;
}

InputEvent resizeEvent(int width, int height) {
  InputEvent event;
  event.type = InputEvent::Type::Resize;
  event.size = COORD{static_cast<SHORT>(width),
                     static_cast<SHORT>(height)};
  return event;
}

BrowserEntry mediaEntry(const char* name) {
  return BrowserEntry(name, std::filesystem::path(name),
                      browser_entry::OpenFile{});
}

std::vector<playback_media_actions::Item> mediaActions() {
  using Action = playback_media_actions::Action;
  return {{Action::Play, "Play"},
          {Action::GenerateSubtitles, "Generate subtitles..."}};
}

}  // namespace

int main() {
  using shell_overlay_stack::Layer;
  bool ok = true;

  shell_overlay_stack::Bounds bounds;
  bounds.width = 50;
  bounds.height = 12;
  bounds.topInset = 1;
  const shell_command_catalog::Catalog catalog =
      shell_command_catalog::build({});

  shell_overlay_stack::Model overlays;
  const shell_overlay_stack::DialogOwner failureOwner =
      overlays.createDialogOwner();
  const shell_overlay_stack::DialogOwner decisionOwner =
      overlays.createDialogOwner();
  ok &= expect(!overlays.active() && !overlays.inputModal() &&
                   overlays.activeLayer() == Layer::None,
               "the overlay stack must start empty");

  ok &= expect(overlays.toggleCommandPalette() &&
                   !overlays.inputModal() &&
                   overlays.activeLayer() == Layer::CommandPalette,
               "the command palette must become the sole active layer");
  ok &=
      expect(overlays.openMediaMenu(mediaEntry("video.mp4"), mediaActions()) &&
                 overlays.activeLayer() == Layer::MediaMenu,
             "opening a media menu must replace the command palette");

  shell_overlay_stack::Interaction interaction =
      overlays.handle(keyEvent(VK_DOWN), bounds, catalog);
  ok &= expect(
      interaction.consumed && interaction.changed && !interaction.mediaCommand,
      "input must be routed to the topmost media menu");
  interaction = overlays.handle(keyEvent(VK_RETURN), bounds, catalog);
  ok &= expect(
      interaction.mediaCommand &&
          interaction.mediaCommand->entry.path == "video.mp4" &&
          interaction.mediaCommand->action ==
              playback_media_actions::Action::GenerateSubtitles &&
          overlays.activeLayer() == Layer::None,
      "media activation must publish one typed command and close the layer");

  overlays.openMediaMenu(mediaEntry("audio.flac"), mediaActions());
  ok &= expect(overlays.toggleCommandPalette() &&
                   overlays.activeLayer() == Layer::CommandPalette,
               "opening the palette must replace an active media menu");
  interaction = overlays.handle(keyEvent(VK_RETURN), bounds, catalog);
  ok &= expect(interaction.paletteIntent &&
                   std::get_if<PlaybackAction>(&*interaction.paletteIntent) &&
                   std::get<PlaybackAction>(*interaction.paletteIntent) ==
                       PlaybackAction::TogglePause &&
                   overlays.activeLayer() == Layer::None,
               "palette activation must publish its typed catalog intent");

  overlays.toggleCommandPalette();
  interaction =
      overlays.handle(inputActionEvent(InputAction::Back), bounds, catalog);
  ok &= expect(interaction.consumed && interaction.changed &&
                   overlays.activeLayer() == Layer::None,
               "Back must dismiss exactly the active overlay layer");

  tui_dialog::Content failureDialog;
  failureDialog.title = "Audio separation failed";
  failureDialog.text.push_back(
      {"Reason: DirectML device was removed", tui_dialog::TextTone::Error});
  failureDialog.buttons = {{0, "Close"}, {1, "Retry"}};
  overlays.toggleCommandPalette();
  const shell_overlay_stack::DialogOpening failureOpening =
      overlays.openDialog(failureOwner, std::move(failureDialog));
  const tui_dialog::DialogId failureDialogId = failureOpening.lease.dialog;
  ok &= expect(failureDialogId &&
                   overlays.activeDialog() == failureOpening.lease &&
                   !failureOpening.replaced &&
                   overlays.activeLayer() == Layer::Dialog,
               "a dialog must replace every less important transient layer");
  ok &= expect(overlays.inputModal(),
               "only a dialog must identify itself as input-modal");
  interaction = overlays.handle(keyEvent(VK_F1), bounds, catalog);
  ok &= expect(
      interaction.consumed && overlays.activeLayer() == Layer::Dialog &&
          !overlays.toggleCommandPalette() &&
          !overlays.openMediaMenu(mediaEntry("blocked.mp4"), mediaActions()),
      "an input-modal dialog must consume unrelated shortcuts");
  interaction = overlays.handle(keyEvent(VK_TAB), bounds, catalog);
  interaction = overlays.handle(keyEvent(VK_RETURN), bounds, catalog);
  ok &= expect(interaction.dialogResolution &&
                   interaction.dialogResolution->lease ==
                       failureOpening.lease &&
                   interaction.dialogResolution->activatedButton == 1 &&
                   !overlays.activeDialog() &&
                   overlays.activeLayer() == Layer::None,
               "dialog buttons must publish a typed result without nesting "
               "an event loop");

  tui_dialog::Content safeDefaultDialog;
  safeDefaultDialog.title = "Cancel task?";
  safeDefaultDialog.buttons = {{2, "Cancel task"}, {3, "Keep running"}};
  safeDefaultDialog.initiallySelectedButton = 3;
  const shell_overlay_stack::DialogOpening safeDefaultOpening =
      overlays.openDialog(decisionOwner, std::move(safeDefaultDialog));
  const tui_dialog::DialogId safeDefaultDialogId =
      safeDefaultOpening.lease.dialog;
  interaction = overlays.handle(resizeEvent(32, 8), {32, 8, 1}, catalog);
  ok &= expect(interaction.consumed && overlays.inputModal() &&
                   overlays.activeLayer() == Layer::Dialog,
               "a resize must be routed to an active dialog using the new "
               "host bounds without dismissing a renderable decision");
  interaction = overlays.handle(keyEvent(VK_RETURN), bounds, catalog);
  ok &=
      expect(interaction.dialogResolution &&
                 interaction.dialogResolution->lease.dialog ==
                     safeDefaultDialogId &&
                 interaction.dialogResolution->lease.owner == decisionOwner &&
                 interaction.dialogResolution->activatedButton == 3 &&
                 overlays.activeLayer() == Layer::None &&
                 !overlays.inputModal(),
             "a dialog must honor an explicitly selected safe default");

  tui_dialog::Content keyboardDialog;
  keyboardDialog.title = "Keyboard grammar";
  keyboardDialog.buttons = {{6, "First"}, {7, "Second"}};
  const shell_overlay_stack::DialogOpening keyboardOpening =
      overlays.openDialog(decisionOwner, std::move(keyboardDialog));
  const tui_dialog::DialogId keyboardDialogId = keyboardOpening.lease.dialog;
  interaction = overlays.handle(keyEvent(VK_TAB), bounds, catalog);
  interaction =
      overlays.handle(keyEvent(VK_TAB, SHIFT_PRESSED), bounds, catalog);
  interaction = overlays.handle(keyEvent(VK_SPACE), bounds, catalog);
  ok &= expect(interaction.dialogResolution &&
                   interaction.dialogResolution->lease.dialog ==
                       keyboardDialogId &&
                   interaction.dialogResolution->activatedButton == 6,
               "dialog buttons must support reverse Tab navigation and Space "
               "activation");

  tui_dialog::Content pointerDialog;
  pointerDialog.title = "Pointer activation";
  pointerDialog.buttons = {{4, "Confirm"}, {5, "Cancel"}};
  tui_dialog::Model pointerModel;
  const tui_dialog::DialogId pointerDialogId =
      pointerModel.open(std::move(pointerDialog));
  const tui_dialog::Bounds pointerBounds{bounds.width, bounds.height,
                                         bounds.topInset};
  const tui_dialog::Layout pointerLayout = pointerModel.layout(pointerBounds);
  const tui_dialog::ButtonBounds& confirm = pointerLayout.buttons.front();
  tui_dialog::Interaction pointerInteraction = pointerModel.handle(
      mouseEvent(MouseEventKind::Press, confirm.x, confirm.y),
      pointerBounds);
  ok &= expect(pointerInteraction.consumed && !pointerInteraction.activation &&
                   pointerModel.active(),
               "a dialog button press must not activate before release");
  pointerInteraction = pointerModel.handle(
      mouseEvent(MouseEventKind::Release, confirm.x, confirm.y),
      pointerBounds);
  ok &= expect(pointerInteraction.activation &&
                   pointerInteraction.activation->dialog == pointerDialogId &&
                   pointerInteraction.activation->button == 4 &&
                   !pointerModel.active(),
               "a dialog button must activate on release over the armed "
               "button");

  tui_dialog::Content resizePointerDialog;
  resizePointerDialog.title = "Resize pointer capture";
  resizePointerDialog.buttons = {{10, "Confirm"}, {11, "Cancel"}};
  tui_dialog::Model resizePointerModel;
  resizePointerModel.open(std::move(resizePointerDialog));
  const tui_dialog::Layout resizePointerLayout =
      resizePointerModel.layout(pointerBounds);
  const tui_dialog::ButtonBounds& resizeConfirm =
      resizePointerLayout.buttons.front();
  pointerInteraction = resizePointerModel.handle(
      mouseEvent(MouseEventKind::Press, resizeConfirm.x, resizeConfirm.y),
      pointerBounds);
  pointerInteraction = resizePointerModel.handle(
      resizeEvent(pointerBounds.width, pointerBounds.height), pointerBounds);
  pointerInteraction = resizePointerModel.handle(
      mouseEvent(MouseEventKind::Release, resizeConfirm.x, resizeConfirm.y),
      pointerBounds);
  ok &= expect(!pointerInteraction.activation && resizePointerModel.active(),
               "resizing between press and release must cancel dialog "
               "activation");

  tui_dialog::Content responsiveDialog;
  responsiveDialog.title = "Cancel separation?";
  responsiveDialog.buttons = {{8, "Cancel task", "Stop"},
                              {9, "Keep running", "Keep"}};
  responsiveDialog.initiallySelectedButton = 9;
  tui_dialog::Model responsiveModel;
  responsiveModel.open(std::move(responsiveDialog));
  const tui_dialog::Layout compactDialog =
      responsiveModel.layout({20, 4, 0});
  ok &= expect(compactDialog.valid && compactDialog.buttons.size() == 2 &&
                   compactDialog.buttons[0].compact &&
                   compactDialog.buttons[1].compact,
               "a short dialog must preserve both actions with explicit "
               "compact labels");
  const tui_dialog::Layout chromeConstrainedDialog =
      responsiveModel.layout({20, 4, 3});
  ok &= expect(chromeConstrainedDialog.valid &&
                   chromeConstrainedDialog.y == 0 &&
                   chromeConstrainedDialog.buttons.size() == 2,
               "a modal dialog must reclaim browser chrome on an extremely "
               "short terminal");
  const tui_dialog::Layout stackedDialog =
      responsiveModel.layout({12, 6, 0});
  ok &= expect(stackedDialog.valid && stackedDialog.buttons.size() == 2 &&
                   stackedDialog.buttons[0].y !=
                       stackedDialog.buttons[1].y,
               "a narrow dialog must vertically reflow complete compact "
               "actions");
  const tui_dialog::Interaction impossibleDialog =
      responsiveModel.handle(keyEvent('A'), {6, 3, 0});
  ok &= expect(impossibleDialog.consumed && !impossibleDialog.changed &&
                   !impossibleDialog.dismissedDialog &&
                   responsiveModel.active(),
               "an unrenderable dialog must preserve the pending decision "
               "without allowing input to click through");
  const tui_dialog::Interaction restoredDialog =
      responsiveModel.handle(resizeEvent(32, 8), {32, 8, 0});
  ok &= expect(restoredDialog.consumed && restoredDialog.changed &&
                   responsiveModel.active() &&
                   responsiveModel.layout({32, 8, 0}).valid,
               "a preserved dialog must reappear after the surface recovers");
  ok &= expect(responsiveModel.dismiss(),
               "the restored test dialog must dismiss explicitly");

  tui_dialog::Content replacementDialog;
  replacementDialog.title = "Stable dialog identity";
  const shell_overlay_stack::DialogOpening replacementOpening =
      overlays.openDialog(decisionOwner, std::move(replacementDialog));
  const tui_dialog::DialogId replacementId = replacementOpening.lease.dialog;
  ok &= expect(!overlays.dismissDialog(failureDialogId) &&
                   overlays.activeLayer() == Layer::Dialog &&
                   overlays.dismissDialog(replacementId) &&
                   overlays.activeLayer() == Layer::None,
               "conditional dismissal must never close a replacement dialog");

  overlays.toggleCommandPalette();
  ok &= expect(overlays.openMediaMenu(mediaEntry("empty.mp4"), {}) &&
                   overlays.activeLayer() == Layer::None,
               "an empty media menu request must still close the prior layer");
  ok &= expect(!overlays.dismiss().changed,
               "dismissing an empty stack must be idempotent");

  tui_dialog::Content ownedDialog;
  ownedDialog.title = "Owned replacement";
  const shell_overlay_stack::DialogOpening firstOwned =
      overlays.openDialog(failureOwner, std::move(ownedDialog));
  tui_dialog::Content replacingDialog;
  replacingDialog.title = "New owner";
  const shell_overlay_stack::DialogOpening secondOwned =
      overlays.openDialog(decisionOwner, std::move(replacingDialog));
  ok &= expect(secondOwned.replaced == firstOwned.lease &&
                   overlays.activeDialog() == secondOwned.lease,
               "opening a dialog must return the exact displaced owner lease");
  interaction = overlays.handle(
      inputActionEvent(InputAction::Back), bounds, catalog);
  ok &= expect(interaction.dialogResolution &&
                   interaction.dialogResolution->lease == secondOwned.lease &&
                   !interaction.dialogResolution->activatedButton,
               "dismissal must resolve only the owner of the active lease");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
