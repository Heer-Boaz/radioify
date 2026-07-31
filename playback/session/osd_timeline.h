#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "playback/overlay/osd_state.h"

namespace playback_session {

class PlaybackOsdTimeline {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  void showControls(TimePoint now, std::chrono::milliseconds duration);
  void showMessage(std::string text, TimePoint now,
                   std::chrono::milliseconds duration);
  [[nodiscard]] bool expire(TimePoint now);
  void clearControls();
  void clear();

  [[nodiscard]] bool controlsVisible() const;
  [[nodiscard]] playback_overlay::PlaybackOsdSnapshot snapshot() const;
  [[nodiscard]] std::optional<TimePoint> nextDeadline() const;

 private:
  mutable std::mutex mutex_;
  bool controlsVisible_ = false;
  TimePoint controlsUntil_ = TimePoint::min();
  std::shared_ptr<const std::string> message_;
  TimePoint messageUntil_ = TimePoint::min();
};

}  // namespace playback_session
