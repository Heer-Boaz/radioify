#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "native_wait_handle.h"

class OpenFileRequests {
 public:
  OpenFileRequests();
  ~OpenFileRequests();

  OpenFileRequests(const OpenFileRequests&) = delete;
  OpenFileRequests& operator=(const OpenFileRequests&) = delete;

  void post(std::filesystem::path file);
  void post(std::vector<std::filesystem::path> files);
  bool poll(std::vector<std::filesystem::path>& out);
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
