#include "terminal_input_sequence.h"

#include <algorithm>
#include <cstddef>

#include "consoleinput.h"

namespace {

constexpr wchar_t kEsc = L'\x1b';

bool parseNumber(const wchar_t* text, size_t length, size_t& offset,
                 int& out) {
  if (offset >= length || text[offset] < L'0' || text[offset] > L'9') {
    return false;
  }
  int value = 0;
  while (offset < length && text[offset] >= L'0' && text[offset] <= L'9') {
    value = value * 10 + static_cast<int>(text[offset] - L'0');
    ++offset;
  }
  out = value;
  return true;
}

DWORD terminalMouseButtons(int buttonCode, bool release) {
  if (release) {
    return 0;
  }

  switch (buttonCode & 0x03) {
    case 0:
      return FROM_LEFT_1ST_BUTTON_PRESSED;
    case 1:
      return FROM_LEFT_2ND_BUTTON_PRESSED;
    case 2:
      return RIGHTMOST_BUTTON_PRESSED;
    default:
      return 0;
  }
}

DWORD terminalMouseModifiers(int buttonCode) {
  DWORD control = 0;
  if ((buttonCode & 0x04) != 0) control |= SHIFT_PRESSED;
  if ((buttonCode & 0x08) != 0) control |= LEFT_ALT_PRESSED;
  if ((buttonCode & 0x10) != 0) control |= LEFT_CTRL_PRESSED;
  return control;
}

WORD csiFinalToVirtualKey(wchar_t final) {
  switch (final) {
    case L'A':
      return VK_UP;
    case L'B':
      return VK_DOWN;
    case L'C':
      return VK_RIGHT;
    case L'D':
      return VK_LEFT;
    case L'H':
      return VK_HOME;
    case L'F':
      return VK_END;
    default:
      return 0;
  }
}

DWORD csiModifierToControl(int modifier) {
  DWORD control = 0;
  if ((modifier & 0x01) != 0) control |= SHIFT_PRESSED;
  if ((modifier & 0x02) != 0) control |= LEFT_ALT_PRESSED;
  if ((modifier & 0x04) != 0) control |= LEFT_CTRL_PRESSED;
  return control;
}

}  // namespace

TerminalInputSequenceParser::Result TerminalInputSequenceParser::feed(
    wchar_t ch, InputEvent& out) {
  if (length_ == 0 && ch != kEsc) {
    return Result::None;
  }
  if (length_ >= sizeof(buffer_) / sizeof(buffer_[0])) {
    reset();
    return Result::Rejected;
  }

  buffer_[length_++] = ch;
  bool complete = false;
  if (parse(out, &complete)) {
    reset();
    return Result::Event;
  }
  if (complete) {
    reset();
    return Result::Rejected;
  }
  return Result::Pending;
}

bool TerminalInputSequenceParser::flushPendingEscape(InputEvent& out) {
  if (length_ != 1 || buffer_[0] != kEsc) {
    return false;
  }
  reset();
  out.type = InputEvent::Type::Key;
  out.key.vk = VK_ESCAPE;
  out.key.ch = 27;
  out.key.control = 0;
  return true;
}

void TerminalInputSequenceParser::reset() {
  length_ = 0;
}

bool TerminalInputSequenceParser::parse(InputEvent& out, bool* complete) {
  *complete = false;
  if (length_ == 0) return false;
  if (buffer_[0] != kEsc) {
    *complete = true;
    return false;
  }
  if (length_ == 1) return false;
  if (buffer_[1] != L'[') {
    *complete = true;
    return false;
  }
  if (length_ == 2) return false;

  if (buffer_[2] == L'<') {
    return parseMouse(out, complete);
  }
  return parseKey(out, complete);
}

bool TerminalInputSequenceParser::parseMouse(InputEvent& out,
                                             bool* complete) const {
  size_t offset = 3;
  int buttonCode = 0;
  int x = 0;
  int y = 0;
  if (!parseNumber(buffer_, length_, offset, buttonCode)) return false;
  if (offset >= length_) return false;
  if (buffer_[offset++] != L';') {
    *complete = true;
    return false;
  }
  if (!parseNumber(buffer_, length_, offset, x)) return false;
  if (offset >= length_) return false;
  if (buffer_[offset++] != L';') {
    *complete = true;
    return false;
  }
  if (!parseNumber(buffer_, length_, offset, y)) return false;
  if (offset >= length_) return false;

  const wchar_t final = buffer_[offset++];
  if (offset != length_) {
    *complete = true;
    return false;
  }
  if (final != L'M' && final != L'm') {
    *complete = true;
    return false;
  }

  const bool wheel = (buttonCode & 0x40) != 0;
  const bool release = final == L'm' || (!wheel && (buttonCode & 0x03) == 3);
  const bool motion = (buttonCode & 0x20) != 0;
  out.type = InputEvent::Type::Mouse;
  out.mouse.pixelX = std::max(0, x - 1);
  out.mouse.pixelY = std::max(0, y - 1);
  out.mouse.hasPixelPosition = true;
  out.mouse.control = terminalMouseModifiers(buttonCode);
  if (wheel) {
    const SHORT delta = ((buttonCode & 0x01) == 0) ? WHEEL_DELTA : -WHEEL_DELTA;
    out.mouse.buttonState =
        static_cast<DWORD>(static_cast<WORD>(delta)) << 16;
    out.mouse.eventFlags = MOUSE_WHEELED;
    return true;
  }
  out.mouse.buttonState = terminalMouseButtons(buttonCode, release);
  out.mouse.eventFlags = motion ? MOUSE_MOVED : 0;
  return true;
}

bool TerminalInputSequenceParser::parseKey(InputEvent& out,
                                           bool* complete) const {
  const wchar_t final = buffer_[length_ - 1];
  if ((final >= L'0' && final <= L'9') || final == L';') {
    return false;
  }

  WORD vk = csiFinalToVirtualKey(final);
  DWORD control = 0;
  if (vk != 0) {
    if (length_ > 3) {
      size_t offset = 2;
      int first = 0;
      int modifier = 0;
      if (!parseNumber(buffer_, length_, offset, first)) {
        *complete = true;
        return false;
      }
      if (offset < length_ - 1 && buffer_[offset++] == L';' &&
          parseNumber(buffer_, length_, offset, modifier) &&
          offset == length_ - 1 && modifier > 0) {
        control = csiModifierToControl(modifier - 1);
      } else {
        *complete = true;
        return false;
      }
    }
    out.type = InputEvent::Type::Key;
    out.key.vk = vk;
    out.key.ch = 0;
    out.key.control = control;
    return true;
  }

  if (final == L'~') {
    size_t offset = 2;
    int keyCode = 0;
    if (!parseNumber(buffer_, length_, offset, keyCode)) {
      *complete = true;
      return false;
    }
    int modifier = 0;
    if (offset < length_ - 1 && buffer_[offset++] == L';' &&
        parseNumber(buffer_, length_, offset, modifier) && modifier > 0) {
      control = csiModifierToControl(modifier - 1);
    }
    if (offset != length_ - 1) {
      *complete = true;
      return false;
    }
    switch (keyCode) {
      case 1:
      case 7:
        vk = VK_HOME;
        break;
      case 4:
      case 8:
        vk = VK_END;
        break;
      case 5:
        vk = VK_PRIOR;
        break;
      case 6:
        vk = VK_NEXT;
        break;
      case 3:
        vk = VK_DELETE;
        break;
      case 2:
        vk = VK_INSERT;
        break;
      default:
        *complete = true;
        return false;
    }
    out.type = InputEvent::Type::Key;
    out.key.vk = vk;
    out.key.ch = 0;
    out.key.control = control;
    return true;
  }

  *complete = true;
  return false;
}
