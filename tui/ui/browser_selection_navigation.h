#pragma once

#include <cstdint>

#include "browser_keymap.h"
#include "browser_model.h"

namespace browser_selection_navigation {

// Applies a resolved browser-navigation gesture as one state transition. The
// repeat count is bounded by the number of entries because additional ticks
// cannot move a clamped selection any further.
bool apply(BrowserState& browser, const GridLayout& layout,
           browser_input::KeyAction action, std::uint32_t repetitions = 1);

}  // namespace browser_selection_navigation
