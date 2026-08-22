#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "open_file_requests.h"
#include "shell_open_mode.h"

ShellOpenMode configuredWindowsShellOpenMode();

bool forwardWindowsShellOpenFile(const std::filesystem::path& file,
                                 OpenPresentationDirective presentation,
                                 uint32_t timeoutMs = 4000);

class WindowsShellOpenServer {
 public:
  explicit WindowsShellOpenServer(OpenFileRequests& requests);
  ~WindowsShellOpenServer();

  WindowsShellOpenServer(const WindowsShellOpenServer&) = delete;
  WindowsShellOpenServer& operator=(const WindowsShellOpenServer&) = delete;

  bool isAcceptingHandoffs() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
