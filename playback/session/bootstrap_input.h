#pragma once

#include <cstdint>
#include <optional>

#include "input_event.h"
#include "playback/input/shortcuts.h"

namespace playback_session_bootstrap_input {

enum class Action : std::uint8_t {
  Cancel,
  QuitApplication,
};

inline std::optional<Action> resolve(const InputEvent& event) {
  if (event.type == InputEvent::Type::Action &&
      event.action == InputAction::Back) {
    return Action::Cancel;
  }
  if (event.type != InputEvent::Type::Key) {
    return std::nullopt;
  }
  if (resolvePlaybackAction(event.key, kPlaybackShortcutContextGlobal) ==
      PlaybackAction::Quit) {
    return Action::QuitApplication;
  }
  if (matchesShortcut(event.key, 'C', 'c', 'C', kShortcutCtrlMask,
                      kShortcutChordForbiddenMask) ||
      event.key.vk == VK_ESCAPE || event.key.vk == VK_BACK) {
    return Action::Cancel;
  }
  return std::nullopt;
}

}  // namespace playback_session_bootstrap_input
