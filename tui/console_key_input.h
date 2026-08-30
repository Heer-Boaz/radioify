#pragma once

#include <array>
#include <cstddef>

#include <windows.h>

#include "input_event.h"

namespace tui_console_key_input {

// Converts the Windows console adapter's KEY_EVENT_RECORD protocol into the
// platform-neutral key lifecycle used by Radioify's input routers. State is
// reset when console focus is lost because Windows need not deliver the
// corresponding key-up record to an unfocused console.
class State {
 public:
  KeyEvent keyDown(const KEY_EVENT_RECORD& record);
  void keyUp(WORD virtualKey);
  void reset();

 private:
  std::array<bool, 256> pressed_{};
};

KeyEvent terminalCharacter(wchar_t character);

}  // namespace tui_console_key_input
