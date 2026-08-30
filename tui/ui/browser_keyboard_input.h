#pragma once

#include <cstdint>
#include <variant>

#include "browser_keymap.h"
#include "browser_model.h"
#include "browser_search.h"
#include "input_event.h"
#include "playback/input/command.h"

namespace browser_keyboard_input {

struct Capabilities {
  bool interactionEnabled = true;
  bool playbackAvailable = false;
};

struct SearchUpdate {
  BrowserSearchUpdate update;
};

struct PlaybackCommand {
  playback_input::Command command;
};

struct BrowserAction {
  browser_input::KeyAction action;
  std::uint32_t repetitions;
};

using Result =
    std::variant<std::monostate, SearchUpdate, PlaybackCommand, BrowserAction>;

// Owns mode-local keyboard precedence for the browser surface. Shell-global
// and system-media commands are resolved before this boundary; within the
// browser, search activation and focused text entry precede contextual
// playback and browser navigation.
Result handle(BrowserState& browser, const InputEvent& event,
              const Capabilities& capabilities);

}  // namespace browser_keyboard_input
