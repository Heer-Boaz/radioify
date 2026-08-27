#include "tui/ui/shell_overlay_stack.h"

#include <cstdlib>
#include <iostream>
#include <vector>

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
  ok &= expect(overlays.openMediaMenu(mediaEntry("video.mp4"),
                                      mediaActions()) &&
                   overlays.activeLayer() == Layer::MediaMenu,
               "opening a media menu must replace the command palette");

  shell_overlay_stack::Interaction interaction =
      overlays.handle(keyEvent(VK_DOWN), bounds, catalog);
  ok &= expect(interaction.consumed && interaction.changed &&
                   !interaction.mediaCommand,
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
  interaction = overlays.handle(inputActionEvent(InputAction::Back), bounds,
                                catalog);
  ok &= expect(interaction.consumed && interaction.changed &&
                   overlays.activeLayer() == Layer::None,
               "Back must dismiss exactly the active overlay layer");

  overlays.toggleCommandPalette();
  ok &= expect(overlays.openMediaMenu(mediaEntry("empty.mp4"), {}) &&
                   overlays.activeLayer() == Layer::None,
               "an empty media menu request must still close the prior layer");
  ok &= expect(!overlays.dismiss(),
               "dismissing an empty stack must be idempotent");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
