#include "windows_console_window.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

void activateWindowsConsoleWindow() {
  HWND hwnd = GetConsoleWindow();
  if (!hwnd) {
    return;
  }
  ShowWindow(hwnd, SW_RESTORE);
  SetForegroundWindow(hwnd);
}

#else

void activateWindowsConsoleWindow() {}

#endif
