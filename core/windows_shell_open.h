#pragma once

#include <filesystem>
#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "shell_open_mode.h"

class OpenFileRequests;

ShellOpenMode configuredWindowsShellOpenMode();

bool forwardWindowsShellOpenFile(const std::filesystem::path& file,
                                 DWORD timeoutMs = 4000);

class WindowsShellOpenServer {
 public:
  explicit WindowsShellOpenServer(OpenFileRequests& requests);
  ~WindowsShellOpenServer();

  WindowsShellOpenServer(const WindowsShellOpenServer&) = delete;
  WindowsShellOpenServer& operator=(const WindowsShellOpenServer&) = delete;

  bool start();
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
