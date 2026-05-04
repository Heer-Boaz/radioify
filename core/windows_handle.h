#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <utility>

class UniqueWindowsHandle {
 public:
  UniqueWindowsHandle() = default;
  explicit UniqueWindowsHandle(HANDLE handle) : handle_(handle) {}
  ~UniqueWindowsHandle() { reset(); }

  UniqueWindowsHandle(const UniqueWindowsHandle&) = delete;
  UniqueWindowsHandle& operator=(const UniqueWindowsHandle&) = delete;

  UniqueWindowsHandle(UniqueWindowsHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}

  UniqueWindowsHandle& operator=(UniqueWindowsHandle&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.handle_, nullptr));
    }
    return *this;
  }

  explicit operator bool() const { return isValid(handle_); }

  HANDLE get() const { return handle_; }

  void reset(HANDLE handle = nullptr) {
    if (isValid(handle_)) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

 private:
  static bool isValid(HANDLE handle) {
    return handle && handle != INVALID_HANDLE_VALUE;
  }

  HANDLE handle_ = nullptr;
};
