#include "transient_message.h"

#include <cassert>
#include <utility>

namespace playback_overlay {

void TransientMessage::show(std::string text, TimePoint now,
                            std::chrono::milliseconds duration) {
  assert(!text.empty());
  assert(duration.count() > 0);

  std::lock_guard<std::mutex> lock(mutex_);
  text_ = std::move(text);
  expiresAt_ = now + duration;
}

bool TransientMessage::visibleAt(TimePoint now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return visibleAtLocked(now);
}

std::optional<std::string> TransientMessage::textAt(TimePoint now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!visibleAtLocked(now)) {
    return std::nullopt;
  }
  return text_;
}

bool TransientMessage::visibleAtLocked(TimePoint now) const {
  return text_.has_value() && now < expiresAt_;
}

}  // namespace playback_overlay
