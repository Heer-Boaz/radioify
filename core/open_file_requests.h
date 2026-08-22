#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "native_wait_handle.h"

enum class OpenPresentationDirective : std::uint8_t {
  InheritActive = 0,
  UseLaunchDefaults = 1,
  TerminalAscii = 2,
  NativeWindowedFramebuffer = 3,
};

struct OpenFilesRequest {
  std::vector<std::filesystem::path> files;
  OpenPresentationDirective presentation =
      OpenPresentationDirective::InheritActive;
};

class OpenFileRequests {
 public:
  OpenFileRequests();
  ~OpenFileRequests();

  OpenFileRequests(const OpenFileRequests&) = delete;
  OpenFileRequests& operator=(const OpenFileRequests&) = delete;

  void post(OpenFilesRequest request);
  bool hasPending() const;
  bool poll(OpenFilesRequest& out);
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
