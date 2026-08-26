#include "tui/ui/browser_media_menu.h"

#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "browser_media_menu_tests: " << message << '\n';
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

std::vector<playback_media_actions::Item> actions() {
  using Action = playback_media_actions::Action;
  return {{Action::Play, "Play"},
          {Action::GenerateSubtitles, "Generate subtitles..."}};
}

}  // namespace

int main() {
  bool ok = true;
  tui_popup_menu::Bounds bounds;
  bounds.width = 40;
  bounds.height = 8;

  tui_browser_media_menu::Model menu;
  ok &= expect(!menu.open(mediaEntry("empty.mp4"), {}) && !menu.active(),
               "an empty catalog must not open a menu");

  ok &= expect(menu.open(mediaEntry("first.mp4"), actions()) && menu.active(),
               "a populated catalog must open the typed menu");
  tui_browser_media_menu::Interaction interaction =
      menu.handle(keyEvent(VK_DOWN), bounds);
  ok &= expect(interaction.consumed && interaction.changed &&
                   !interaction.command,
               "navigation must remain owned by the popup model");
  interaction = menu.handle(keyEvent(VK_RETURN), bounds);
  ok &= expect(interaction.command &&
                   interaction.command->entry.path == "first.mp4" &&
                   interaction.command->action ==
                       playback_media_actions::Action::GenerateSubtitles &&
                   interaction.dismissed && !menu.active(),
               "activation must return the source and action as one command");

  menu.open(mediaEntry("stale.mp4"), actions());
  menu.open(mediaEntry("current.mp4"),
            {{playback_media_actions::Action::EditVideo, "Edit video"}});
  interaction = menu.handle(keyEvent(VK_RETURN), bounds);
  ok &= expect(interaction.command &&
                   interaction.command->entry.path == "current.mp4" &&
                   interaction.command->action ==
                       playback_media_actions::Action::EditVideo,
               "reopening must replace subject and catalog atomically");

  menu.open(mediaEntry("dismissed.mp4"), actions());
  interaction = menu.handle(inputActionEvent(InputAction::Back), bounds);
  ok &= expect(interaction.consumed && interaction.dismissed &&
                   !interaction.command && !menu.active() &&
                   !menu.dismiss(),
               "dismissal must clear both popup and bound command state");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
