#include "consoleinput.h"

bool ConsoleInput::hasInputFocus() const {
  // Trust console focus events for pseudoconsole hosts (Windows Terminal,
  // ConPTY, etc.) where foreground-window ownership checks are unreliable.
  return focusActive_;
}

void ConsoleInput::init() {
  handle_ = GetStdHandle(STD_INPUT_HANDLE);
  if (handle_ == INVALID_HANDLE_VALUE) return;
  if (!GetConsoleMode(handle_, &originalMode_)) return;
  DWORD mode = originalMode_;
  mode |= ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
  mode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
  mode |= ENABLE_EXTENDED_FLAGS;
  if (!SetConsoleMode(handle_, mode)) return;
  focusActive_ = true;
  active_ = true;
}

void ConsoleInput::restore() {
  if (active_) {
    SetConsoleMode(handle_, originalMode_);
  }
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
