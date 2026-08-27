#pragma once

#include <chrono>
#include <optional>

namespace wake_schedule {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Deadline = std::optional<TimePoint>;

inline void include(Deadline& deadline, TimePoint candidate) {
  if (!deadline || candidate < *deadline) deadline = candidate;
}

inline void include(Deadline& deadline, const Deadline& candidate) {
  if (candidate) include(deadline, *candidate);
}

}  // namespace wake_schedule
