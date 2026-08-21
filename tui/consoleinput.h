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

#include "core/native_wait_handle.h"
#include "input_event.h"
#include "mouse_double_click_tracker.h"
#include "terminal_input_sequence.h"

struct BreadcrumbLine;

class ConsoleInput {
 public:
  ConsoleInput() = default;

  ConsoleInput(const ConsoleInput&) = delete;
  ConsoleInput& operator=(const ConsoleInput&) = delete;

  void init();
  void restore();
  void setCellPixelSize(double width, double height);
  bool poll(InputEvent& out);
  bool active() const;
  NativeWaitHandle waitHandle() const;

 private:
  void enableTerminalMouseInput();
  void disableTerminalMouseInput();
  void updateTerminalGridSize();
  void mapPixelMousePosition(MouseEvent& mouse) const;
  void normalizeTerminalMouseGesture(MouseEvent& mouse);
  void normalizeTerminalEvent(InputEvent& event);
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
  DWORD consoleMouseButtonState_ = 0;
  bool terminalMouseInput_ = false;
  std::wstring originalConsoleTitle_;
  std::wstring activeConsoleTitle_;
  TerminalInputSequenceParser terminalParser_;
  terminal_input::MouseDoubleClickTracker terminalDoubleClickTracker_;
};

#endif
