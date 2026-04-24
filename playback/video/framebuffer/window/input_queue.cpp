#include "input_queue.h"

#include <utility>

void WindowInputQueue::push(InputEvent ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back(std::move(ev));
}

bool WindowInputQueue::poll(InputEvent& ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) {
    return false;
  }
  ev = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

void WindowInputQueue::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
}
