#pragma once

#include <memory>

#include "core/waitable_signal.h"

// Copyable producer token for an owner-scoped wake event. Producers can wake
// their owner without gaining the ability to consume or reset its event.
class WakeNotifier {
 public:
  WakeNotifier() = default;

  void notify() const {
    if (const std::shared_ptr<WaitableSignal> signal = signal_.lock()) {
      signal->signal();
    }
  }

 private:
  explicit WakeNotifier(const std::shared_ptr<WaitableSignal>& signal)
      : signal_(signal) {}

  std::weak_ptr<WaitableSignal> signal_;

  friend class WakeEvent;
};

// Single consumer event used to fan in any number of owner-bound producers.
// Notifiers become harmless no-ops after the event owner is destroyed.
class WakeEvent {
 public:
  WakeEvent() : signal_(std::make_shared<WaitableSignal>()) {}

  WakeEvent(const WakeEvent&) = delete;
  WakeEvent& operator=(const WakeEvent&) = delete;

  WakeNotifier notifier() const { return WakeNotifier(signal_); }
  bool consume() { return signal_->consume(); }
  NativeWaitHandle nativeWaitHandle() const {
    return signal_->nativeWaitHandle();
  }

 private:
  std::shared_ptr<WaitableSignal> signal_;
};
