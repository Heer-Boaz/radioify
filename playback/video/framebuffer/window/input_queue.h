#pragma once

#include <cstddef>
#include <deque>
#include <mutex>

#include "core/native_wait_handle.h"
#include "core/waitable_signal.h"
#include "input_event.h"

class WindowInputQueue {
 public:
  static constexpr std::size_t kMaximumPendingEvents = 256;

  void push(InputEvent ev);
  bool poll(InputEvent& ev);
  void clear();
  NativeWaitHandle nativeWaitHandle() const;

 private:
  std::mutex mutex_;
  std::deque<InputEvent> queue_;
  WaitableSignal ready_;
};
