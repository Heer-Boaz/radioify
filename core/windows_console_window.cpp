#include "windows_console_window.h"

#include "windows_app_resources.h"

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

constexpr DWORD kConsoleTitleBufferChars = 512;

std::wstring activeRadioifyConsoleTitle() {
  return RADIOIFY_APP_NAME_W L" [" + std::to_wstring(GetCurrentProcessId()) +
         L"]";
}

}  // namespace

bool radioifyConsoleWindowTitleMatches(std::wstring_view title) {
  const std::wstring activeTitle = activeRadioifyConsoleTitle();
  return !activeTitle.empty() && title.find(activeTitle) != std::wstring::npos;
}

bool isRadioifyConsoleForegroundWindow() {
  HWND foreground = GetForegroundWindow();
  if (!foreground) {
    return false;
  }

  if (HWND console = GetConsoleWindow()) {
    if (foreground == console) {
      return true;
    }
  }

  wchar_t foregroundTitle[kConsoleTitleBufferChars]{};
  const int length =
      GetWindowTextW(foreground, foregroundTitle, kConsoleTitleBufferChars);
  if (length <= 0) {
    return false;
  }

  return radioifyConsoleWindowTitleMatches(
      std::wstring_view(foregroundTitle, static_cast<size_t>(length)));
}

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
bool isRadioifyConsoleForegroundWindow() { return false; }
bool radioifyConsoleWindowTitleMatches(std::wstring_view) { return false; }

#endif
