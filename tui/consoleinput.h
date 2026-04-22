#ifndef CONSOLEINPUT_H
#define CONSOLEINPUT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

struct InputEvent {
  enum class Type {
    None,
    Key,
    Mouse,
    Resize,
    FileDrop,
  };

  Type type = Type::None;
  KeyEvent key{};
  MouseEvent mouse{};
  COORD size{};
  FileDropEvent fileDrop;
};

class ConsoleInput {
 public:
  void init();
  void restore();
  void setCellPixelSize(double width, double height);
  bool poll(InputEvent& out);
  bool active() const;
  HANDLE waitHandle() const;

 private:
  bool hasInputFocus() const;
  void enableTerminalMouseInput();
  void disableTerminalMouseInput();
  void updateTerminalGridSize();
  void mapPixelMousePosition(MouseEvent& mouse) const;
  bool pollTerminalInputStream(InputEvent& out);
  bool handleTerminalInputCharacter(wchar_t ch, InputEvent& out);
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  HANDLE output_ = INVALID_HANDLE_VALUE;
  DWORD originalMode_ = 0;
  int columns_ = 80;
  int rows_ = 25;
  double cellPixelWidth_ = 1.0;
  double cellPixelHeight_ = 1.0;
  bool active_ = false;
  bool terminalMouseInput_ = false;
  bool xButton1Prev_ = false;
  bool xButton2Prev_ = false;
  bool focusActive_ = true;
  TerminalInputSequenceParser terminalParser_;
};

#endif
