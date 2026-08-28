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

InputEvent keyEvent(WORD key) {
  InputEvent event;
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
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
  ok &= expect(!overlays.active() && overlays.activeLayer() == Layer::None,
               "the overlay stack must start empty");

  ok &= expect(overlays.toggleCommandPalette() &&
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
  const tui_dialog::DialogId failureDialogId =
      overlays.openDialog(std::move(failureDialog));
  ok &= expect(failureDialogId && overlays.activeLayer() == Layer::Dialog,
               "a dialog must replace every less important transient layer");
  interaction = overlays.handle(keyEvent(VK_F1), bounds, catalog);
  ok &= expect(
      interaction.consumed && overlays.activeLayer() == Layer::Dialog &&
          !overlays.toggleCommandPalette() &&
          !overlays.openMediaMenu(mediaEntry("blocked.mp4"), mediaActions()),
      "an input-modal dialog must consume unrelated shortcuts");
  interaction = overlays.handle(keyEvent(VK_TAB), bounds, catalog);
  interaction = overlays.handle(keyEvent(VK_RETURN), bounds, catalog);
  ok &= expect(interaction.dialogActivation &&
                   interaction.dialogActivation->dialog == failureDialogId &&
                   interaction.dialogActivation->button == 1 &&
                   overlays.activeLayer() == Layer::None,
               "dialog buttons must publish a typed result without nesting "
               "an event loop");

  tui_dialog::Content safeDefaultDialog;
  safeDefaultDialog.title = "Cancel task?";
  safeDefaultDialog.buttons = {{2, "Cancel task"}, {3, "Keep running"}};
  safeDefaultDialog.initiallySelectedButton = 3;
  const tui_dialog::DialogId safeDefaultDialogId =
      overlays.openDialog(std::move(safeDefaultDialog));
  interaction = overlays.handle(keyEvent(VK_RETURN), bounds, catalog);
  ok &=
      expect(interaction.dialogActivation &&
                 interaction.dialogActivation->dialog == safeDefaultDialogId &&
                 interaction.dialogActivation->button == 3 &&
                 overlays.activeLayer() == Layer::None,
             "a dialog must honor an explicitly selected safe default");

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
      mouseEvent(MouseEventKind::Press, confirm.x, pointerLayout.buttonY),
      pointerBounds);
  ok &= expect(pointerInteraction.consumed && !pointerInteraction.activation &&
                   pointerModel.active(),
               "a dialog button press must not activate before release");
  pointerInteraction = pointerModel.handle(
      mouseEvent(MouseEventKind::Release, confirm.x, pointerLayout.buttonY),
      pointerBounds);
  ok &= expect(pointerInteraction.activation &&
                   pointerInteraction.activation->dialog == pointerDialogId &&
                   pointerInteraction.activation->button == 4 &&
                   !pointerModel.active(),
               "a dialog button must activate on release over the armed "
               "button");

  tui_dialog::Content replacementDialog;
  replacementDialog.title = "Stable dialog identity";
  const tui_dialog::DialogId replacementId =
      overlays.openDialog(std::move(replacementDialog));
  ok &= expect(!overlays.dismissDialog(failureDialogId) &&
                   overlays.activeLayer() == Layer::Dialog &&
                   overlays.dismissDialog(replacementId) &&
                   overlays.activeLayer() == Layer::None,
               "conditional dismissal must never close a replacement dialog");

  overlays.toggleCommandPalette();
  ok &= expect(overlays.openMediaMenu(mediaEntry("empty.mp4"), {}) &&
                   overlays.activeLayer() == Layer::None,
               "an empty media menu request must still close the prior layer");
  ok &= expect(!overlays.dismiss(),
               "dismissing an empty stack must be idempotent");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
