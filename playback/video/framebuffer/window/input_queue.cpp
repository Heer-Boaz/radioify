#include "input_queue.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace {

bool shouldCoalesceMouseMove(const InputEvent& queuedTail,
                             const InputEvent& incoming) {
  return queuedTail.type == InputEvent::Type::Mouse &&
         incoming.type == InputEvent::Type::Mouse &&
         queuedTail.mouse.kind == MouseEventKind::Move &&
         incoming.mouse.kind == MouseEventKind::Move &&
         queuedTail.mouse.buttons == incoming.mouse.buttons;
}

bool shouldCoalesceResize(const InputEvent& queuedTail,
                          const InputEvent& incoming) {
  return queuedTail.type == InputEvent::Type::Resize &&
         incoming.type == InputEvent::Type::Resize;
}

bool shouldCoalesceKeyRepeat(const InputEvent& queuedTail,
                             const InputEvent& incoming) {
  return queuedTail.type == InputEvent::Type::Key &&
         incoming.type == InputEvent::Type::Key &&
         isAutoRepeat(queuedTail.key) && isAutoRepeat(incoming.key) &&
         queuedTail.key.vk == incoming.key.vk &&
         queuedTail.key.ch == incoming.key.ch &&
         queuedTail.key.control == incoming.key.control;
}

bool isCoalescibleStateEvent(const InputEvent& event) {
  return (event.type == InputEvent::Type::Mouse &&
          event.mouse.kind == MouseEventKind::Move) ||
         event.type == InputEvent::Type::Resize ||
         (event.type == InputEvent::Type::Key &&
          isAutoRepeat(event.key));
}

bool mergeCoalescibleEvent(InputEvent& queued, const InputEvent& incoming) {
  if (shouldCoalesceMouseMove(queued, incoming) ||
      shouldCoalesceResize(queued, incoming)) {
    queued = incoming;
    return true;
  }
  const bool twoKeyRepeats =
      queued.type == InputEvent::Type::Key &&
      incoming.type == InputEvent::Type::Key && isAutoRepeat(queued.key) &&
      isAutoRepeat(incoming.key);
  if (!twoKeyRepeats) {
    return false;
  }
  if (!shouldCoalesceKeyRepeat(queued, incoming)) {
    // A repeat is held-state rather than a discrete command. When another
    // held key becomes current, replace the pending repeat so opposite seeks
    // or navigation keys cannot execute as stale alternating backlog.
    queued = incoming;
    return true;
  }
  const std::uint32_t maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  const std::uint32_t queuedCount = keyPressCount(queued.key);
  const std::uint32_t incomingCount = keyPressCount(incoming.key);
  queued.key.repeatCount =
      incomingCount > maximum - queuedCount ? maximum
                                            : queuedCount + incomingCount;
  return true;
}

}  // namespace

void WindowInputQueue::push(InputEvent ev) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (isCoalescibleStateEvent(ev)) {
    // Coalescible state may cross other coalescible state, but never a
    // discrete input barrier. This keeps the newest mouse/resize state and at
    // most one current counted repeat without reordering clicks, initial
    // key presses, file drops, or explicit actions.
    for (auto it = queue_.rbegin(); it != queue_.rend(); ++it) {
      if (!isCoalescibleStateEvent(*it)) break;
      InputEvent merged = *it;
      if (mergeCoalescibleEvent(merged, ev)) {
        queue_.erase(std::next(it).base());
        queue_.push_back(std::move(merged));
        return;
      }
      if (ev.type == InputEvent::Type::Mouse &&
          ev.mouse.kind == MouseEventKind::Move &&
          it->type == InputEvent::Type::Mouse &&
          it->mouse.kind == MouseEventKind::Move) {
        // Different button state is an ordered pointer transition, not stale
        // motion that a later move may cross.
        break;
      }
    }
  }

  if (queue_.size() >= kMaximumPendingEvents) {
    // Preserve discrete intent preferentially. Under an all-discrete flood,
    // retain the most recent bounded window of user intent instead of letting
    // delayed input execute without limit after rendering catches up.
    const auto staleState =
        std::find_if(queue_.begin(), queue_.end(), isCoalescibleStateEvent);
    if (staleState != queue_.end()) {
      queue_.erase(staleState);
    } else {
      queue_.pop_front();
    }
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
