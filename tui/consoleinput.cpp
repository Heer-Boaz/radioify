#include "consoleinput.h"

#include <algorithm>
#include <cwchar>

namespace {

bool writeTerminalSequence(HANDLE output, const wchar_t* sequence) {
  if (output == INVALID_HANDLE_VALUE || !sequence) return false;
  DWORD written = 0;
  DWORD length = static_cast<DWORD>(wcslen(sequence));
  return WriteConsoleW(output, sequence, length, &written, nullptr) &&
         written == length;
}

}  // namespace

bool ConsoleInput::hasInputFocus() const {
  // Trust console focus events for pseudoconsole hosts (Windows Terminal,
  // ConPTY, etc.) where foreground-window ownership checks are unreliable.
  return focusActive_;
}

void ConsoleInput::init() {
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
  updateTerminalGridSize();
  focusActive_ = true;
  active_ = true;
}

void ConsoleInput::restore() {
  disableTerminalMouseInput();
  if (active_) {
    SetConsoleMode(handle_, originalMode_);
  }
}

void ConsoleInput::setCellPixelSize(double width, double height) {
  if (width > 0.0) {
    cellPixelWidth_ = width;
  }
  if (height > 0.0) {
    cellPixelHeight_ = height;
  }
  enableTerminalMouseInput();
}

void ConsoleInput::enableTerminalMouseInput() {
  if (terminalMouseInput_) return;
  if (writeTerminalSequence(output_, L"\x1b[?1003h\x1b[?1016h")) {
    terminalMouseInput_ = true;
  }
}

void ConsoleInput::disableTerminalMouseInput() {
  if (!terminalMouseInput_) return;
  writeTerminalSequence(output_, L"\x1b[?1016l\x1b[?1003l");
  terminalMouseInput_ = false;
  terminalParser_.reset();
}

void ConsoleInput::updateTerminalGridSize() {
  if (output_ == INVALID_HANDLE_VALUE) return;
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(output_, &info)) return;
  columns_ = std::max(1, static_cast<int>(info.srWindow.Right -
                                          info.srWindow.Left + 1));
  rows_ = std::max(1, static_cast<int>(info.srWindow.Bottom -
                                       info.srWindow.Top + 1));
}

void ConsoleInput::mapPixelMousePosition(MouseEvent& mouse) const {
  if (!mouse.hasPixelPosition) return;
  const double cellW = std::max(1.0, cellPixelWidth_);
  const double cellH = std::max(1.0, cellPixelHeight_);
  mouse.unitWidth = cellW;
  mouse.unitHeight = cellH;
  const int gx = std::clamp(static_cast<int>(mouse.pixelX / cellW), 0,
                            std::max(0, columns_ - 1));
  const int gy = std::clamp(static_cast<int>(mouse.pixelY / cellH), 0,
                            std::max(0, rows_ - 1));
  mouse.pos.X = static_cast<SHORT>(gx);
  mouse.pos.Y = static_cast<SHORT>(gy);
}

bool ConsoleInput::poll(InputEvent& out) {
  if (!active_) return false;

  // Fallback for terminal hosts that don't forward XBUTTON mouse events
  // through ReadConsoleInput. Detect side-button edges via async key state.
  bool focused = hasInputFocus();
  bool x1Down = (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
  bool x2Down = (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
  bool x1Pressed = focused && x1Down && !xButton1Prev_;
  bool x2Pressed = focused && x2Down && !xButton2Prev_;
  xButton1Prev_ = x1Down;
  xButton2Prev_ = x2Down;
  if (x1Pressed) {
    out.type = InputEvent::Type::Key;
    out.key.vk = VK_BROWSER_BACK;
    out.key.ch = 0;
    out.key.control = 0;
    return true;
  }
  if (x2Pressed) {
    out.type = InputEvent::Type::Key;
    out.key.vk = VK_BROWSER_FORWARD;
    out.key.ch = 0;
    out.key.control = 0;
    return true;
  }

  DWORD count = 0;
  if (!GetNumberOfConsoleInputEvents(handle_, &count) || count == 0)
    return false;
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
      if (kev.uChar.UnicodeChar != 0) {
        InputEvent parsed{};
        TerminalInputSequenceParser::Result parsedResult =
            terminalParser_.feed(kev.uChar.UnicodeChar, parsed);
        if (parsedResult == TerminalInputSequenceParser::Result::Event) {
          if (parsed.type == InputEvent::Type::Mouse) {
            mapPixelMousePosition(parsed.mouse);
          }
          out = parsed;
          return true;
        }
        if (parsedResult == TerminalInputSequenceParser::Result::Pending) {
          DWORD remaining = 0;
          if ((!GetNumberOfConsoleInputEvents(handle_, &remaining) ||
               remaining == 0) &&
              terminalParser_.flushPendingEscape(out)) {
            return true;
          }
          count--;
          continue;
        }
        if (parsedResult == TerminalInputSequenceParser::Result::Rejected) {
          count--;
          continue;
        }
      }
      out.type = InputEvent::Type::Key;
      out.key.vk = kev.wVirtualKeyCode;
      out.key.ch = static_cast<char>(kev.uChar.AsciiChar);
      out.key.control = kev.dwControlKeyState;
      return true;
    }
    if (rec.EventType == MOUSE_EVENT) {
      const auto& mev = rec.Event.MouseEvent;
      if (mev.dwEventFlags == 0) {
        DWORD sideMask = FROM_LEFT_2ND_BUTTON_PRESSED |
                         FROM_LEFT_3RD_BUTTON_PRESSED |
                         FROM_LEFT_4TH_BUTTON_PRESSED;
        if ((mev.dwButtonState & sideMask) != 0) {
          out.type = InputEvent::Type::Key;
          if ((mev.dwButtonState & FROM_LEFT_2ND_BUTTON_PRESSED) != 0) {
            out.key.vk = VK_BROWSER_BACK;
          } else {
            out.key.vk = VK_BROWSER_FORWARD;
          }
          out.key.ch = 0;
          out.key.control = mev.dwControlKeyState;
          return true;
        }
      }
      out.type = InputEvent::Type::Mouse;
      out.mouse.pos = mev.dwMousePosition;
      out.mouse.buttonState = mev.dwButtonState;
      out.mouse.eventFlags = mev.dwEventFlags;
      out.mouse.control = mev.dwControlKeyState;
      return true;
    }
    if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
      updateTerminalGridSize();
      out.type = InputEvent::Type::Resize;
      out.size = rec.Event.WindowBufferSizeEvent.dwSize;
      return true;
    }
    if (rec.EventType == FOCUS_EVENT) {
      focusActive_ = rec.Event.FocusEvent.bSetFocus != FALSE;
      count--;
      continue;
    }
    count--;
  }
  return false;
}

bool ConsoleInput::active() const { return active_; }

HANDLE ConsoleInput::waitHandle() const {
  if (!active_) return nullptr;
  return handle_;
}

std::vector<DriveEntry> listDriveEntries() {
  std::vector<DriveEntry> drives;
#ifdef _WIN32
  DWORD mask = GetLogicalDrives();
  if (mask == 0) return drives;
  for (int i = 0; i < 26; ++i) {
    if ((mask & (1u << i)) == 0) continue;
    char letter = static_cast<char>('A' + i);
    std::string label;
    label.push_back(letter);
    label.push_back(':');
    std::string root;
    root.push_back(letter);
    root.append(":\\");
    drives.push_back(DriveEntry{label, std::filesystem::path(root)});
  }
#endif
  return drives;
}
