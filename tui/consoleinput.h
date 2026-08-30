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
#include "console_key_input.h"
#include "input_event.h"
#include "playback/input/media_keys.h"
#include "terminal_input_sequence.h"

struct BreadcrumbLine;

class ConsoleInput {
 public:
  ConsoleInput() = default;

  ConsoleInput(const ConsoleInput&) = delete;
  ConsoleInput& operator=(const ConsoleInput&) = delete;

  void init();
  void restore();
  void enableTerminalMouseInput();
  void setSystemMediaCommandOwner(SystemMediaCommandOwner owner);
  bool poll(InputEvent& out);
  bool active() const;
  NativeWaitHandle waitHandle() const;

 private:
  void disableTerminalMouseInput();
  bool ownsForegroundConsoleWindow() const;
  bool pollTerminalInputStream(InputEvent& out);
  bool handleTerminalInputCharacter(wchar_t ch, InputEvent& out);
  bool pollBrowserButtonFallback(InputEvent& out);
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  HANDLE output_ = INVALID_HANDLE_VALUE;
  DWORD originalMode_ = 0;
  bool active_ = false;
  bool focusActive_ = true;
  bool xButton1Down_ = false;
  bool xButton2Down_ = false;
  tui_console_key_input::State keyPressState_;
  DWORD consoleMouseButtonState_ = 0;
  bool terminalMouseInput_ = false;
  SystemMediaCommandOwner systemMediaCommandOwner_ =
      SystemMediaCommandOwner::LocalInputFallback;
  std::wstring originalConsoleTitle_;
  std::wstring activeConsoleTitle_;
  TerminalInputSequenceParser terminalParser_;
};

#endif
