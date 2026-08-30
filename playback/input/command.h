#pragma once

#include <variant>

#include "playback/input/shortcut_types.h"

namespace playback_input {

struct SeekToRatio {
  double ratio = 0.0;
};

struct SeekBySteps {
  int steps = 0;
};

struct AdjustVolume {
  float delta = 0.0f;
};

// Presentation surfaces emit one typed command stream. Key bindings, pointer
// controls, PiP, and command-palette actions therefore share dispatch policy
// instead of copying callback wiring per surface.
using Command =
    std::variant<PlaybackAction, SeekToRatio, SeekBySteps, AdjustVolume>;

}  // namespace playback_input
