#pragma once

#include "consoleinput.h"

inline constexpr DWORD kShortcutCtrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
inline constexpr DWORD kShortcutAltMask = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
inline constexpr DWORD kShortcutShiftMask = SHIFT_PRESSED;
inline constexpr DWORD kShortcutTextForbiddenMask =
    kShortcutCtrlMask | kShortcutAltMask;
inline constexpr DWORD kShortcutChordForbiddenMask =
    kShortcutAltMask | kShortcutShiftMask;

inline bool matchesAsciiShortcut(const KeyEvent& key, WORD vk, char lower,
                                 char upper) {
  // A zero ASCII alias means "no text fallback for this binding".
  if (key.vk == vk) {
    return true;
  }
  if (lower != 0 && key.ch == lower) {
    return true;
  }
  if (upper != 0 && key.ch == upper) {
    return true;
  }
  return false;
}

// Modifier masks use any-bit semantics: a required mask matches if any listed
// bit is present, and a forbidden mask rejects if any listed bit is present.
inline bool matchesShortcut(const KeyEvent& key, WORD vk, char lower,
                            char upper, DWORD requiredModifierMask = 0,
                            DWORD forbiddenModifierMask = 0) {
  if (requiredModifierMask != 0 && (key.control & requiredModifierMask) == 0) {
    return false;
  }
  if (forbiddenModifierMask != 0 &&
      (key.control & forbiddenModifierMask) != 0) {
    return false;
  }
  return matchesAsciiShortcut(key, vk, lower, upper);
}
