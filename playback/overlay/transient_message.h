#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace playback_overlay {

class TransientMessage {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  void show(std::string text, TimePoint now,
            std::chrono::milliseconds duration);
  bool visibleAt(TimePoint now) const;
  std::optional<std::string> textAt(TimePoint now) const;

 private:
  bool visibleAtLocked(TimePoint now) const;

  mutable std::mutex mutex_;
  std::optional<std::string> text_;
  TimePoint expiresAt_ = TimePoint::min();
};

}  // namespace playback_overlay
