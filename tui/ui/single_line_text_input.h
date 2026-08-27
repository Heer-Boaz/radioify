#pragma once

#include <cstdint>
#include <string>

#include "input_event.h"

namespace single_line_text_input {

enum class Intent : std::uint8_t {
  None,
  Commit,
  Cancel,
};

struct EditResult {
  bool handled = false;
  bool changed = false;
  Intent intent = Intent::None;
};

// Applies the editing keys shared by the browser search fields and command
// palette. Navigation remains with the owning widget.
EditResult edit(std::string& text, const KeyEvent& key);

}  // namespace single_line_text_input
