#include "videowindow_input_queue.h"

#include <utility>

void VideoWindowInputQueue::push(InputEvent ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back(std::move(ev));
}

bool VideoWindowInputQueue::poll(InputEvent& ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) {
    return false;
  }
  ev = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

void VideoWindowInputQueue::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
}
