#include "single_line_text_input.h"

#include "shortcut_match.h"

namespace single_line_text_input {

EditResult edit(std::string& text, const KeyEvent& key) {
  if (key.vk == VK_ESCAPE) {
    return {.handled = true, .intent = Intent::Cancel};
  }
  if (key.vk == VK_RETURN) {
    return {.handled = true, .intent = Intent::Commit};
  }
  if (key.vk == VK_BACK) {
    if (text.empty()) {
      return {.handled = true};
    }
    text.pop_back();
    return {.handled = true, .changed = true};
  }
  const DWORD modifierKeys = key.control & kShortcutTextForbiddenMask;
  const bool altGr =
      (modifierKeys & LEFT_CTRL_PRESSED) != 0 &&
      (modifierKeys & RIGHT_ALT_PRESSED) != 0 &&
      (modifierKeys & (RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED)) == 0;
  if ((modifierKeys == 0 || altGr) && key.ch >= 32) {
    text.push_back(key.ch);
    return {.handled = true, .changed = true};
  }
  return {};
}

}  // namespace single_line_text_input
