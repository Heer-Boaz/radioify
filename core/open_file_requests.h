#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "native_wait_handle.h"

enum class OpenVideoMode {
  Inherit,
  Ascii,
  Framebuffer,
};

struct OpenFilesRequest {
  std::vector<std::filesystem::path> files;
  OpenVideoMode videoMode = OpenVideoMode::Inherit;
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
