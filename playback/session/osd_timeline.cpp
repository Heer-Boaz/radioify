#include "osd_timeline.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <utility>

namespace playback_session {

void PlaybackOsdTimeline::showControls(
    TimePoint now, std::chrono::milliseconds duration) {
  assert(duration.count() > 0);

  std::lock_guard<std::mutex> lock(mutex_);
  controlsVisible_ = true;
  controlsUntil_ = now + duration;
}

void PlaybackOsdTimeline::showMessage(
    std::string text, TimePoint now, std::chrono::milliseconds duration) {
  assert(!text.empty());
  assert(duration.count() > 0);

  auto message = std::make_shared<const std::string>(std::move(text));
  std::lock_guard<std::mutex> lock(mutex_);
  message_ = std::move(message);
  messageUntil_ = now + duration;
}

bool PlaybackOsdTimeline::expire(TimePoint now) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool changed = false;
  if (controlsVisible_ && now >= controlsUntil_) {
    controlsVisible_ = false;
    controlsUntil_ = TimePoint::min();
    changed = true;
  }
  if (message_ && now >= messageUntil_) {
    message_.reset();
    messageUntil_ = TimePoint::min();
    changed = true;
  }
  return changed;
}

void PlaybackOsdTimeline::clearControls() {
  std::lock_guard<std::mutex> lock(mutex_);
  controlsVisible_ = false;
  controlsUntil_ = TimePoint::min();
}

void PlaybackOsdTimeline::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  controlsVisible_ = false;
  controlsUntil_ = TimePoint::min();
  message_.reset();
  messageUntil_ = TimePoint::min();
}

bool PlaybackOsdTimeline::controlsVisible() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return controlsVisible_;
}

playback_overlay::PlaybackOsdSnapshot PlaybackOsdTimeline::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {controlsVisible_, message_};
}

std::optional<PlaybackOsdTimeline::TimePoint>
PlaybackOsdTimeline::nextDeadline() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (controlsVisible_ && message_) {
    return std::min(controlsUntil_, messageUntil_);
  }
  if (controlsVisible_) {
    return controlsUntil_;
  }
  if (message_) {
    return messageUntil_;
  }
  return std::nullopt;
}

}  // namespace playback_session
