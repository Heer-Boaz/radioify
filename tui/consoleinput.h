#ifndef CONSOLEINPUT_H
#define CONSOLEINPUT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "playback/input/input_action.h"
#include "terminal_input_sequence.h"
#include "file_drop_event.h"

struct BreadcrumbLine;

struct KeyEvent {
  WORD vk = 0;
  char ch = 0;
  DWORD control = 0;
};

struct MouseEvent {
  COORD pos{};
  DWORD buttonState = 0;
  DWORD eventFlags = 0;
  DWORD control = 0;
  bool hasPixelPosition = false;
  int pixelX = 0;
  int pixelY = 0;
  double unitWidth = 1.0;
  double unitHeight = 1.0;
};

inline constexpr DWORD kInputEventWindowMouseFlag = 0x80000000u;

inline bool isWindowMouseEvent(const MouseEvent& mouse) {
  return (mouse.control & kInputEventWindowMouseFlag) != 0;
}

inline void markWindowMouseEvent(MouseEvent& mouse) {
  mouse.control |= kInputEventWindowMouseFlag;
}

inline void clearWindowMouseEvent(MouseEvent& mouse) {
  mouse.control &= ~kInputEventWindowMouseFlag;
}

struct InputEvent {
  enum class Type {
    None,
    Key,
    Action,
    Mouse,
    Resize,
    FileDrop,
  };

  Type type = Type::None;
  KeyEvent key{};
  InputAction action = InputAction::Back;
  MouseEvent mouse{};
  COORD size{};
  FileDropEvent fileDrop;
};

inline InputEvent inputActionEvent(InputAction action) {
  InputEvent ev{};
  ev.type = InputEvent::Type::Action;
  ev.action = action;
  return ev;
}

class ConsoleInput {
 public:
  void init();
  void restore();
  void setCellPixelSize(double width, double height);
  bool poll(InputEvent& out);
  bool active() const;
  HANDLE waitHandle() const;

 private:
  void enableTerminalMouseInput();
  void disableTerminalMouseInput();
  void updateTerminalGridSize();
  void mapPixelMousePosition(MouseEvent& mouse) const;
  bool ownsForegroundConsoleWindow() const;
  bool pollTerminalInputStream(InputEvent& out);
  bool handleTerminalInputCharacter(wchar_t ch, InputEvent& out);
  bool pollBrowserButtonFallback(InputEvent& out);
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  HANDLE output_ = INVALID_HANDLE_VALUE;
  DWORD originalMode_ = 0;
  int columns_ = 80;
  int rows_ = 25;
  double cellPixelWidth_ = 1.0;
  double cellPixelHeight_ = 1.0;
  bool active_ = false;
  bool focusActive_ = true;
  bool xButton1Down_ = false;
  bool xButton2Down_ = false;
  bool terminalMouseInput_ = false;
  std::wstring originalConsoleTitle_;
  std::wstring activeConsoleTitle_;
  TerminalInputSequenceParser terminalParser_;
};

#endif
