#include "core/windows_app_resources.h"
#include "core/windows_console_window.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "windows_console_window_tests: " << message << '\n';
    return false;
  }
  return true;
}

std::wstring activeRadioifyConsoleTitleForCurrentProcess() {
#ifdef _WIN32
  return RADIOIFY_APP_NAME_W L" [" + std::to_wstring(GetCurrentProcessId()) +
         L"]";
#else
  return {};
#endif
}

}  // namespace

int main() {
  bool ok = true;

  const std::wstring activeTitle =
      activeRadioifyConsoleTitleForCurrentProcess();
  ok &= expect(radioifyConsoleWindowTitleMatches(activeTitle),
               "active Radioify console title must match");
  ok &= expect(radioifyConsoleWindowTitleMatches(
                   std::wstring(L"host - ") + activeTitle + L" - tab"),
               "host windows containing the active title must match");
  ok &= expect(!radioifyConsoleWindowTitleMatches(L""),
               "empty titles must not match");
  ok &= expect(!radioifyConsoleWindowTitleMatches(
                   RADIOIFY_APP_NAME_W L" [1]"),
               "titles for another Radioify process must not match");

  return ok ? 0 : 1;
}
