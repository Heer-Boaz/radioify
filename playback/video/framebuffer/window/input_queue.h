#pragma once

#include <deque>
#include <mutex>

#include "core/native_wait_handle.h"
#include "core/waitable_signal.h"
#include "consoleinput.h"

class WindowInputQueue {
 public:
  void push(InputEvent ev);
  bool poll(InputEvent& ev);
  void clear();
  NativeWaitHandle nativeWaitHandle() const;

 private:
  std::mutex mutex_;
  std::deque<InputEvent> queue_;
  WaitableSignal ready_;
};
