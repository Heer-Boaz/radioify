#include "consoleinput.h"

#include <cwchar>
#include <optional>
#include <string>
#include <utility>

#include "core/windows_app_resources.h"
#include "core/windows_console_window.h"

namespace {

constexpr DWORD kTerminalInputSequenceWaitMs = 10;
constexpr DWORD kConsoleTitleBufferChars = 512;
// SGR 1006 and SGR-Pixels 1016 use the same wire format. Reset 1016 first so
// every parsed SGR coordinate has the single, explicit meaning "terminal cell".
constexpr wchar_t kEnableTerminalMouseInput[] =
    L"\x1b[?1016l\x1b[?1006h\x1b[?1003h";
constexpr wchar_t kDisableTerminalMouseInput[] =
    L"\x1b[?1003l\x1b[?1006l\x1b[?1016l";
constexpr DWORD kMouseButtonMask =
    FROM_LEFT_1ST_BUTTON_PRESSED | RIGHTMOST_BUTTON_PRESSED |
    FROM_LEFT_2ND_BUTTON_PRESSED | FROM_LEFT_3RD_BUTTON_PRESSED |
    FROM_LEFT_4TH_BUTTON_PRESSED;
constexpr DWORD kPointerButtonMask =
    FROM_LEFT_1ST_BUTTON_PRESSED | RIGHTMOST_BUTTON_PRESSED |
    FROM_LEFT_2ND_BUTTON_PRESSED;
constexpr DWORD kSideButtonMask =
    FROM_LEFT_3RD_BUTTON_PRESSED | FROM_LEFT_4TH_BUTTON_PRESSED;

bool writeTerminalSequence(HANDLE output, const wchar_t* sequence) {
  if (output == INVALID_HANDLE_VALUE || !sequence) return false;
  DWORD written = 0;
  DWORD length = static_cast<DWORD>(wcslen(sequence));
  return WriteConsoleW(output, sequence, length, &written, nullptr) &&
         written == length;
}

bool isPrintableAscii(wchar_t ch) {
  return ch >= 0x20 && ch <= 0x7e;
}

void assignKeyEventFromCharacter(KeyEvent& out, WORD rawVk, DWORD rawControl,
                                 wchar_t ch) {
  out.vk = rawVk;
  out.ch = 0;
  out.control = rawControl;

  if (ch == 0) return;

  if (ch >= 1 && ch <= 26 && ch != L'\b' && ch != L'\t' && ch != L'\n' &&
      ch != L'\r') {
    out.vk = static_cast<WORD>('A' + ch - 1);
    out.control |= LEFT_CTRL_PRESSED;
    return;
  }

  switch (ch) {
    case L'\x1b':
      out.vk = VK_ESCAPE;
      return;
    case L'\b':
    case L'\x7f':
      out.vk = VK_BACK;
      return;
    case L'\t':
      out.vk = VK_TAB;
      return;
    case L'\r':
    case L'\n':
      out.vk = VK_RETURN;
      return;
    case L' ':
      out.vk = VK_SPACE;
      out.ch = ' ';
      return;
    default:
      break;
  }

  if (ch >= L'a' && ch <= L'z') {
    out.vk = static_cast<WORD>('A' + ch - L'a');
  } else if (ch >= L'A' && ch <= L'Z') {
    out.vk = static_cast<WORD>(ch);
  } else if (ch >= L'0' && ch <= L'9') {
    out.vk = static_cast<WORD>(ch);
  } else if (ch == L'[') {
    out.vk = VK_OEM_4;
  } else if (ch == L']') {
    out.vk = VK_OEM_6;
  } else if (ch == L',') {
    out.vk = VK_OEM_COMMA;
  } else if (ch == L'.') {
    out.vk = VK_OEM_PERIOD;
  } else if (ch == L'/') {
    out.vk = VK_DIVIDE;
  }

  if (isPrintableAscii(ch)) {
    out.ch = static_cast<char>(ch);
  }
}

void assignKeyEventFromConsoleRecord(KeyEvent& out,
                                     const KEY_EVENT_RECORD& key) {
  wchar_t ch = key.uChar.UnicodeChar;
  if (ch == 0) {
    ch = static_cast<unsigned char>(key.uChar.AsciiChar);
  }
  assignKeyEventFromCharacter(out, key.wVirtualKeyCode, key.dwControlKeyState,
                              ch);
}

void assignKeyEventFromTerminalCharacter(KeyEvent& out, wchar_t ch) {
  assignKeyEventFromCharacter(out, 0, 0, ch);
}

std::optional<InputAction> inputActionFromVirtualKey(WORD vk) {
  if (vk == VK_BROWSER_BACK) {
    return InputAction::Back;
  }
  if (vk == VK_BROWSER_FORWARD) {
    return InputAction::Forward;
  }
  return std::nullopt;
}

constexpr MouseButtons mouseButtonsFromConsoleState(DWORD state) {
  MouseButtons buttons = MouseButtons::None;
  if ((state & FROM_LEFT_1ST_BUTTON_PRESSED) != 0) {
    buttons = buttons | MouseButtons::Left;
  }
  if ((state & FROM_LEFT_2ND_BUTTON_PRESSED) != 0) {
    buttons = buttons | MouseButtons::Middle;
  }
  if ((state & RIGHTMOST_BUTTON_PRESSED) != 0) {
    buttons = buttons | MouseButtons::Right;
  }
  return buttons;
}

constexpr MouseButton mouseButtonFromConsoleState(DWORD state) {
  if ((state & FROM_LEFT_1ST_BUTTON_PRESSED) != 0) {
    return MouseButton::Left;
  }
  if ((state & FROM_LEFT_2ND_BUTTON_PRESSED) != 0) {
    return MouseButton::Middle;
  }
  if ((state & RIGHTMOST_BUTTON_PRESSED) != 0) {
    return MouseButton::Right;
  }
  return MouseButton::None;
}

static_assert(mouseButtonFromConsoleState(
                  FROM_LEFT_2ND_BUTTON_PRESSED) == MouseButton::Middle);
static_assert((kSideButtonMask & FROM_LEFT_2ND_BUTTON_PRESSED) == 0);

MouseEventKind mouseEventKind(const MOUSE_EVENT_RECORD& event) {
  switch (event.dwEventFlags) {
    case 0:
      return MouseEventKind::Unknown;
    case DOUBLE_CLICK:
      // A console double-click record is the second physical press. Gesture
      // meaning belongs to the browser or playback surface that receives it.
      return MouseEventKind::Press;
    case MOUSE_MOVED:
      return MouseEventKind::Move;
    case MOUSE_WHEELED:
      return MouseEventKind::VerticalWheel;
    case MOUSE_HWHEELED:
      return MouseEventKind::HorizontalWheel;
    default:
      return MouseEventKind::Unknown;
  }
}

int mouseWheelDelta(const MOUSE_EVENT_RECORD& event) {
  return event.dwEventFlags == MOUSE_WHEELED ||
                 event.dwEventFlags == MOUSE_HWHEELED
             ? static_cast<SHORT>(HIWORD(event.dwButtonState))
             : 0;
}

}  // namespace

void ConsoleInput::init() {
  consoleMouseButtonState_ = 0;
  handle_ = GetStdHandle(STD_INPUT_HANDLE);
  output_ = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle_ == INVALID_HANDLE_VALUE) return;
  if (!GetConsoleMode(handle_, &originalMode_)) return;
  DWORD mode = originalMode_;
  mode |= ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT |
          ENABLE_VIRTUAL_TERMINAL_INPUT;
  mode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
  mode |= ENABLE_EXTENDED_FLAGS;
  if (!SetConsoleMode(handle_, mode)) return;
  wchar_t title[kConsoleTitleBufferChars]{};
  const DWORD titleLength = GetConsoleTitleW(title, kConsoleTitleBufferChars);
  if (titleLength > 0) {
    originalConsoleTitle_.assign(title, titleLength);
  }
  activeConsoleTitle_ =
      RADIOIFY_APP_NAME_W L" [" + std::to_wstring(GetCurrentProcessId()) +
      L"]";
  SetConsoleTitleW(activeConsoleTitle_.c_str());
  active_ = true;
}

void ConsoleInput::restore() {
  disableTerminalMouseInput();
  consoleMouseButtonState_ = 0;
  if (active_) {
    SetConsoleMode(handle_, originalMode_);
  }
  if (!originalConsoleTitle_.empty()) {
    SetConsoleTitleW(originalConsoleTitle_.c_str());
  }
}

void ConsoleInput::enableTerminalMouseInput() {
  if (terminalMouseInput_) return;
  if (writeTerminalSequence(output_, kEnableTerminalMouseInput)) {
    terminalMouseInput_ = true;
  }
}

void ConsoleInput::setSystemMediaCommandOwner(
    SystemMediaCommandOwner owner) {
  systemMediaCommandOwner_ = owner;
}

void ConsoleInput::disableTerminalMouseInput() {
  if (!terminalMouseInput_) return;
  writeTerminalSequence(output_, kDisableTerminalMouseInput);
  terminalMouseInput_ = false;
  terminalParser_.reset();
}

bool ConsoleInput::ownsForegroundConsoleWindow() const {
  return focusActive_ && isRadioifyConsoleForegroundWindow();
}

bool ConsoleInput::handleTerminalInputCharacter(wchar_t ch, InputEvent& out) {
  if (ch == 0) return false;

  InputEvent parsed{};
  TerminalInputSequenceParser::Result parsedResult =
      terminalParser_.feed(ch, parsed);
  if (parsedResult == TerminalInputSequenceParser::Result::Event) {
    out = std::move(parsed);
    return true;
  }
  if (parsedResult == TerminalInputSequenceParser::Result::Pending ||
      parsedResult == TerminalInputSequenceParser::Result::Rejected) {
    return false;
  }

  out.type = InputEvent::Type::Key;
  assignKeyEventFromTerminalCharacter(out.key, ch);
  return true;
}

bool ConsoleInput::pollTerminalInputStream(InputEvent& out) {
  for (DWORD waitMs = 0;; waitMs = kTerminalInputSequenceWaitMs) {
    if (WaitForSingleObject(handle_, waitMs) != WAIT_OBJECT_0) {
      break;
    }

    wchar_t ch = 0;
    DWORD read = 0;
    if (!ReadConsoleW(handle_, &ch, 1, &read, nullptr) || read == 0) {
      break;
    }
    if (handleTerminalInputCharacter(ch, out)) {
      return true;
    }
  }
  return terminalParser_.flushPendingEscape(out);
}

bool ConsoleInput::pollBrowserButtonFallback(InputEvent& out) {
  if (!ownsForegroundConsoleWindow()) {
    xButton1Down_ = false;
    xButton2Down_ = false;
    return false;
  }

  const bool xButton1Down = (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
  const bool xButton2Down = (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
  const bool xButton1Pressed = xButton1Down && !xButton1Down_;
  const bool xButton2Pressed = xButton2Down && !xButton2Down_;
  xButton1Down_ = xButton1Down;
  xButton2Down_ = xButton2Down;

  if (xButton1Pressed) {
    out = inputActionEvent(InputAction::Back);
    return true;
  }
  if (xButton2Pressed) {
    out = inputActionEvent(InputAction::Forward);
    return true;
  }
  return false;
}

bool ConsoleInput::poll(InputEvent& out) {
  if (!active_) return false;

  DWORD count = 0;
  if (!GetNumberOfConsoleInputEvents(handle_, &count) || count == 0) {
    return pollTerminalInputStream(out) || pollBrowserButtonFallback(out);
  }
  while (count > 0) {
    INPUT_RECORD rec{};
    DWORD read = 0;
    if (!ReadConsoleInput(handle_, &rec, 1, &read) || read == 0) return false;
    if (rec.EventType == KEY_EVENT) {
      const auto& kev = rec.Event.KeyEvent;
      if (!kev.bKeyDown) {
        count--;
        continue;
      }
      if (!shouldDispatchLocalVirtualKey(kev.wVirtualKeyCode,
                                         systemMediaCommandOwner_)) {
        count--;
        continue;
      }
      if (auto action = inputActionFromVirtualKey(kev.wVirtualKeyCode)) {
        out = inputActionEvent(*action);
        return true;
      }
      if (kev.uChar.UnicodeChar != 0) {
        InputEvent parsed{};
        TerminalInputSequenceParser::Result parsedResult =
            terminalParser_.feed(kev.uChar.UnicodeChar, parsed);
        if (parsedResult == TerminalInputSequenceParser::Result::Event) {
          out = std::move(parsed);
          return true;
        }
        if (parsedResult == TerminalInputSequenceParser::Result::Pending) {
          DWORD remaining = 0;
          if (!GetNumberOfConsoleInputEvents(handle_, &remaining)) {
            return false;
          }
          if (remaining == 0 &&
              WaitForSingleObject(handle_, kTerminalInputSequenceWaitMs) ==
                  WAIT_OBJECT_0) {
            GetNumberOfConsoleInputEvents(handle_, &remaining);
          }
          if (remaining == 0) {
            if (pollTerminalInputStream(out)) {
              return true;
            }
            return terminalParser_.flushPendingEscape(out);
          }
          count = remaining;
          continue;
        }
        if (parsedResult == TerminalInputSequenceParser::Result::Rejected) {
          count--;
          continue;
        }
      }
      out.type = InputEvent::Type::Key;
      assignKeyEventFromConsoleRecord(out.key, kev);
      return true;
    }
    if (rec.EventType == MOUSE_EVENT) {
      const auto& mev = rec.Event.MouseEvent;
      const DWORD buttonState = mev.dwButtonState & kMouseButtonMask;
      const DWORD pressed = buttonState & ~consoleMouseButtonState_;
      const DWORD released = consoleMouseButtonState_ & ~buttonState;
      MouseEventKind kind = mouseEventKind(mev);
      if (mev.dwEventFlags == 0) {
        kind = pressed != 0 ? MouseEventKind::Press
                            : (released != 0 ? MouseEventKind::Release
                                             : MouseEventKind::Unknown);
      }
      consoleMouseButtonState_ = buttonState;
      xButton1Down_ =
          (buttonState & FROM_LEFT_3RD_BUTTON_PRESSED) != 0;
      xButton2Down_ =
          (buttonState & FROM_LEFT_4TH_BUTTON_PRESSED) != 0;
      if (kind == MouseEventKind::Unknown) {
        count--;
        continue;
      }

      const DWORD sideButtonsPressed = pressed & kSideButtonMask;
      if (kind == MouseEventKind::Press && sideButtonsPressed != 0) {
        out.type = InputEvent::Type::Action;
        if ((sideButtonsPressed & FROM_LEFT_3RD_BUTTON_PRESSED) != 0) {
          out.action = InputAction::Back;
        } else {
          out.action = InputAction::Forward;
        }
        return true;
      }

      DWORD changedButtonState = 0;
      if (kind == MouseEventKind::Press) {
        changedButtonState = pressed & kPointerButtonMask;
      } else if (kind == MouseEventKind::Release) {
        changedButtonState = released & kPointerButtonMask;
      }
      const MouseButton changedButton =
          mouseButtonFromConsoleState(changedButtonState);
      if ((kind == MouseEventKind::Press ||
           kind == MouseEventKind::Release) &&
          changedButton == MouseButton::None) {
        count--;
        continue;
      }

      out.type = InputEvent::Type::Mouse;
      out.mouse.pos = mev.dwMousePosition;
      out.mouse.buttons = mouseButtonsFromConsoleState(buttonState);
      out.mouse.button = changedButton;
      out.mouse.kind = kind;
      out.mouse.wheelDelta = mouseWheelDelta(mev);
      out.mouse.control = mev.dwControlKeyState;
      return true;
    }
    if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
      out.type = InputEvent::Type::Resize;
      out.size = rec.Event.WindowBufferSizeEvent.dwSize;
      return true;
    }
    if (rec.EventType == FOCUS_EVENT) {
      focusActive_ = rec.Event.FocusEvent.bSetFocus != FALSE;
      if (!focusActive_) {
        xButton1Down_ = false;
        xButton2Down_ = false;
        consoleMouseButtonState_ = 0;
      }
      count--;
      continue;
    }
    count--;
  }
  return pollBrowserButtonFallback(out);
}

bool ConsoleInput::active() const { return active_; }

NativeWaitHandle ConsoleInput::waitHandle() const {
  if (!active_) return {};
  return NativeWaitHandle(handle_);
}
