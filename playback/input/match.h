#pragma once

#include <cstdint>
#include <optional>

#include "input_event.h"
#include "playback/input/command.h"
#include "playback/input/shortcuts.h"

namespace playback_input {

// Converts an input binding to its semantic command only. Presentation and
// dispatch feedback remain responsibilities of the consuming session/surface.
inline std::optional<Command> matchShortcut(
    const InputEvent& event,
    std::uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                     kPlaybackShortcutContextShared,
    std::optional<PlaybackShortcutScope> requiredScope = std::nullopt) {
  const std::optional<PlaybackActionMatch> match =
      resolvePlaybackActionMatch(event, shortcutContexts, requiredScope);
  if (!match) return std::nullopt;
  return commandForShortcut(*match);
}

}  // namespace playback_input
