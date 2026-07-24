#include "input_queue.h"

#include <utility>

namespace {

bool shouldCoalesceMouseMove(const InputEvent& queuedTail,
                             const InputEvent& incoming) {
  return queuedTail.type == InputEvent::Type::Mouse &&
         incoming.type == InputEvent::Type::Mouse &&
         queuedTail.mouse.eventFlags == MOUSE_MOVED &&
         incoming.mouse.eventFlags == MOUSE_MOVED &&
         queuedTail.mouse.buttonState == incoming.mouse.buttonState;
}

}  // namespace

void WindowInputQueue::push(InputEvent ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!queue_.empty() && shouldCoalesceMouseMove(queue_.back(), ev)) {
    queue_.back() = std::move(ev);
    return;
  }
  const bool wasEmpty = queue_.empty();
  queue_.push_back(std::move(ev));
  if (wasEmpty) {
    ready_.signal();
  }
}

bool WindowInputQueue::poll(InputEvent& ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) {
    return false;
  }
  ev = std::move(queue_.front());
  queue_.pop_front();
  if (queue_.empty()) {
    ready_.clear();
  }
  return true;
}

void WindowInputQueue::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
  ready_.clear();
}

NativeWaitHandle WindowInputQueue::nativeWaitHandle() const {
  return ready_.nativeWaitHandle();
}
