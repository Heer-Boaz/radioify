#include "app_common.h"
#include "crash_handler.h"
#include "core/windows_app_identity.h"
#include "tui/tui.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

std::string wideToUtf8(const wchar_t* value) {
  if (!value || value[0] == L'\0') {
    return {};
  }
  const int length =
      WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) {
    return {};
  }
  std::string result(static_cast<size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr,
                      nullptr);
  return result;
}

Options parseWindowsCommandLineUtf8(int fallbackArgc, char** fallbackArgv) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv || argc <= 0) {
    return parseArgs(fallbackArgc, fallbackArgv);
  }

  std::vector<std::string> utf8Args;
  utf8Args.reserve(static_cast<size_t>(argc));
  std::vector<char*> argvUtf8;
  argvUtf8.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    utf8Args.push_back(wideToUtf8(argv[i]));
    argvUtf8.push_back(utf8Args.back().data());
  }

  Options options = parseArgs(argc, argvUtf8.data());
  LocalFree(argv);
  return options;
}

}  // namespace
#endif

int main(int argc, char** argv) {
  initializeWindowsAppIdentity();
  installCrashHandler();
#ifdef _WIN32
  Options o = parseWindowsCommandLineUtf8(argc, argv);
#else
  Options o = parseArgs(argc, argv);
#endif
  return runTui(o);
}
