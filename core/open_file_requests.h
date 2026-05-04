#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class OpenFileRequests {
 public:
  OpenFileRequests();
  ~OpenFileRequests();

  OpenFileRequests(const OpenFileRequests&) = delete;
  OpenFileRequests& operator=(const OpenFileRequests&) = delete;

  void post(std::filesystem::path file);
  void post(std::vector<std::filesystem::path> files);
  bool poll(std::vector<std::filesystem::path>& out);
  HANDLE waitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
