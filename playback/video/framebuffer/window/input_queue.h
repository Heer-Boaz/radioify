#pragma once

#include <deque>
#include <mutex>

#include "consoleinput.h"

class WindowInputQueue {
 public:
  void push(InputEvent ev);
  bool poll(InputEvent& ev);
  void clear();

 private:
  std::mutex mutex_;
  std::deque<InputEvent> queue_;
};
