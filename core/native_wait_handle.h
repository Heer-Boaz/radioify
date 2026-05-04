#pragma once

#include <cstdint>

class NativeWaitHandle {
 public:
  NativeWaitHandle() = default;
  explicit NativeWaitHandle(void* handle) : handle_(handle) {}

  explicit operator bool() const { return isValid(); }

  void* get() const { return handle_; }

 private:
  bool isValid() const {
    return handle_ &&
           handle_ != reinterpret_cast<void*>(static_cast<intptr_t>(-1));
  }

  void* handle_ = nullptr;
};
