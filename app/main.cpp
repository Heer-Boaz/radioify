#include "app_common.h"
#include "application.h"
#include "core/utf8.h"
#include "core/windows_app_identity.h"
#include "crash_handler.h"
#include "playback/video/analysis/inference_worker.h"

#include <cstring>
#include <utility>

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
    utf8Args.push_back(wideToUtf8Lossy(argv[i]));
    argvUtf8.push_back(utf8Args.back().data());
  }

  Options options = parseArgs(argc, argvUtf8.data());
  LocalFree(argv);
  return options;
}

}  // namespace
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  // The isolated inference process shares our executable, but must never start
  // the player, register shell activation, or initialize playback devices.
  if (argc > 1 && std::strcmp(argv[1], "--analysis-worker") == 0) {
    int workerArgc = 0;
    LPWSTR* workerArgv = CommandLineToArgvW(GetCommandLineW(), &workerArgc);
    if (!workerArgv) return 1;
    const std::filesystem::path workspace =
        workerArgc == 3 ? std::filesystem::path(workerArgv[2])
                        : std::filesystem::path{};
    LocalFree(workerArgv);
    if (workspace.empty()) return 1;
    return playback_video_analysis::inferenceWorkerMain(workspace);
  }
#endif
  initializeWindowsAppIdentity();
  installCrashHandler();
#ifdef _WIN32
  Options o = parseWindowsCommandLineUtf8(argc, argv);
#else
  Options o = parseArgs(argc, argv);
#endif
  return runApplication(std::move(o));
}
