#include "consoleinput.h"

void ConsoleInput::init() {
  handle_ = GetStdHandle(STD_INPUT_HANDLE);
  if (handle_ == INVALID_HANDLE_VALUE) return;
  if (!GetConsoleMode(handle_, &originalMode_)) return;
  DWORD mode = originalMode_;
  mode |= ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
  mode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
  mode |= ENABLE_EXTENDED_FLAGS;
  if (!SetConsoleMode(handle_, mode)) return;
  active_ = true;
}

void ConsoleInput::restore() {
  if (active_) {
    SetConsoleMode(handle_, originalMode_);
  }
}

bool ConsoleInput::poll(InputEvent& out) {
  if (!active_) return false;
  DWORD count = 0;
  if (!GetNumberOfConsoleInputEvents(handle_, &count) || count == 0) return false;
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
    count--;
  }
  return false;
}

bool ConsoleInput::active() const { return active_; }
