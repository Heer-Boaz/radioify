#include "shell_keyboard_input.h"

#include <utility>

#include "playback/input/match.h"
#include "tui/shell_shortcuts.h"

namespace shell_keyboard_input {

std::optional<Command> resolve(const InputEvent& event,
                               const Context& context) {
  if (context.inputModal) return std::nullopt;

  if (tui_shell_shortcuts::resolve(
          event, tui_shell_shortcuts::context(
                     tui_shell_shortcuts::Context::Browser)) ==
      tui_shell_shortcuts::Action::ToggleCommandPalette) {
    return ToggleCommandPalette{};
  }

  if (std::optional<playback_input::Command> command =
          playback_input::matchShortcut(event,
                                        kPlaybackShortcutContextGlobal)) {
    return PlaybackCommand{std::move(*command)};
  }

  if (context.playbackAvailable) {
    if (std::optional<playback_input::Command> command =
            playback_input::matchShortcut(
                event, kPlaybackShortcutContextShared,
                PlaybackShortcutScope::SystemMedia)) {
      return PlaybackCommand{std::move(*command)};
    }
  }
  return std::nullopt;
}

}  // namespace shell_keyboard_input
