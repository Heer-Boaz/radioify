#pragma once

#include <mutex>

#include "native_wait_handle.h"
#include "windows_handle.h"

class WaitableSignal {
 public:
  WaitableSignal();

  WaitableSignal(const WaitableSignal&) = delete;
  WaitableSignal& operator=(const WaitableSignal&) = delete;

  void signal();
  bool consume();
  void clear();
  NativeWaitHandle nativeWaitHandle() const;

 private:
  UniqueWindowsHandle event_;
  mutable std::mutex mutex_;
  bool signaled_ = false;
};
