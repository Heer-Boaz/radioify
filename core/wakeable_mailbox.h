#pragma once

#include <mutex>
#include <utility>
#include <vector>

#include "core/native_wait_handle.h"
#include "core/wake_event.h"

// Multi-producer, single-consumer mailbox with an owner-scoped native wake
// handle. The consumer drains complete batches; producers never gain the
// ability to consume or reset the wake event.
template <typename Message>
class WakeableMailbox {
 public:
  WakeableMailbox() : notifier_(wakeEvent_.notifier()) {}

  WakeableMailbox(const WakeableMailbox&) = delete;
  WakeableMailbox& operator=(const WakeableMailbox&) = delete;

  void publish(Message message) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.push_back(std::move(message));
    }
    notifier_.notify();
  }

  // Must only be called by the mailbox owner. Consuming before acquiring the
  // queue lock prevents a producer from leaving an unread message without a
  // corresponding wake. A concurrent publish can at most cause one harmless
  // extra wake cycle.
  std::vector<Message> drain() {
    wakeEvent_.consume();
    std::vector<Message> messages;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      messages.swap(pending_);
    }
    return messages;
  }

  NativeWaitHandle nativeWaitHandle() const {
    return wakeEvent_.nativeWaitHandle();
  }

 private:
  WakeEvent wakeEvent_;
  WakeNotifier notifier_;
  std::mutex mutex_;
  std::vector<Message> pending_;
};
