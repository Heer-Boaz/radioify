#pragma once

#include <optional>
#include <variant>

#include "input_event.h"
#include "playback/input/command.h"

namespace shell_keyboard_input {

struct Context {
  bool inputModal = false;
  bool playbackAvailable = false;
};

struct ToggleCommandPalette {};

struct PlaybackCommand {
  playback_input::Command command;
};

using Command = std::variant<ToggleCommandPalette, PlaybackCommand>;

// Resolves application-level accelerators before non-modal focus owners. A
// modal dialog is the sole shell layer allowed to suspend this command route.
std::optional<Command> resolve(const InputEvent& event,
                               const Context& context);

}  // namespace shell_keyboard_input
