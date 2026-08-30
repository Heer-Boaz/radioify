#include "console_key_input.h"

#include <algorithm>

namespace tui_console_key_input {
namespace {

bool isPrintableAscii(wchar_t ch) {
  return ch >= 0x20 && ch <= 0x7e;
}

KeyEvent keyFromCharacter(WORD rawVk, DWORD rawControl, wchar_t ch) {
  KeyEvent result{};
  result.vk = rawVk;
  result.control = rawControl;

  if (ch == 0) return result;

  if (ch >= 1 && ch <= 26 && ch != L'\b' && ch != L'\t' && ch != L'\n' &&
      ch != L'\r') {
    result.vk = static_cast<WORD>('A' + ch - 1);
    result.control |= LEFT_CTRL_PRESSED;
    return result;
  }

  switch (ch) {
    case L'\x1b':
      result.vk = VK_ESCAPE;
      return result;
    case L'\b':
    case L'\x7f':
      result.vk = VK_BACK;
      return result;
    case L'\t':
      result.vk = VK_TAB;
      return result;
    case L'\r':
    case L'\n':
      result.vk = VK_RETURN;
      return result;
    case L' ':
      result.vk = VK_SPACE;
      result.ch = ' ';
      return result;
    default:
      break;
  }

  if (ch >= L'a' && ch <= L'z') {
    result.vk = static_cast<WORD>('A' + ch - L'a');
  } else if (ch >= L'A' && ch <= L'Z') {
    result.vk = static_cast<WORD>(ch);
  } else if (ch >= L'0' && ch <= L'9') {
    result.vk = static_cast<WORD>(ch);
  } else if (ch == L'[') {
    result.vk = VK_OEM_4;
  } else if (ch == L']') {
    result.vk = VK_OEM_6;
  } else if (ch == L',') {
    result.vk = VK_OEM_COMMA;
  } else if (ch == L'.') {
    result.vk = VK_OEM_PERIOD;
  } else if (ch == L'/') {
    result.vk = VK_DIVIDE;
  }

  if (isPrintableAscii(ch)) {
    result.ch = static_cast<char>(ch);
  }
  return result;
}

}  // namespace

KeyEvent State::keyDown(const KEY_EVENT_RECORD& record) {
  wchar_t character = record.uChar.UnicodeChar;
  if (character == 0) {
    character = static_cast<unsigned char>(record.uChar.AsciiChar);
  }
  KeyEvent result =
      keyFromCharacter(record.wVirtualKeyCode, record.dwControlKeyState,
                       character);

  const std::size_t index = record.wVirtualKeyCode;
  if (index > 0 && index < pressed_.size()) {
    result.pressKind = pressed_[index] ? KeyPressKind::AutoRepeat
                                       : KeyPressKind::Initial;
    pressed_[index] = true;
  }
  result.repeatCount = std::max<std::uint32_t>(1, record.wRepeatCount);
  return result;
}

void State::keyUp(WORD virtualKey) {
  const std::size_t index = virtualKey;
  if (index > 0 && index < pressed_.size()) {
    pressed_[index] = false;
  }
}

void State::reset() { pressed_.fill(false); }

KeyEvent terminalCharacter(wchar_t character) {
  return keyFromCharacter(0, 0, character);
}

}  // namespace tui_console_key_input
