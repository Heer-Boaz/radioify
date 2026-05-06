#include "waitable_signal.h"

#include <stdexcept>

WaitableSignal::WaitableSignal()
    : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
  if (!event_) {
    throw std::runtime_error("Failed to create waitable signal event");
  }
}

void WaitableSignal::signal() {
  std::lock_guard<std::mutex> lock(mutex_);
  signaled_ = true;
  SetEvent(event_.get());
}

bool WaitableSignal::consume() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!signaled_) {
    return false;
  }
  signaled_ = false;
  ResetEvent(event_.get());
  return true;
}

void WaitableSignal::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  signaled_ = false;
  ResetEvent(event_.get());
}

NativeWaitHandle WaitableSignal::nativeWaitHandle() const {
  return NativeWaitHandle(event_.get());
}
