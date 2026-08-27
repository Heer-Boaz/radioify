#include "tui/ui/browser_wake_schedule.h"

#include <chrono>

namespace browser_wake_schedule {
namespace {

using namespace std::chrono_literals;

constexpr auto kSearchCaretBlinkInterval = 500ms;
constexpr auto kMelodyMonitorRefreshInterval = 50ms;
constexpr auto kTransportProgressRefreshInterval = 100ms;
constexpr auto kAudioPictureInPictureRefreshInterval = 100ms;

wake_schedule::TimePoint nextSearchCaretPhase(
    wake_schedule::TimePoint now) {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());
  const auto completedPhases = elapsed / kSearchCaretBlinkInterval;
  return wake_schedule::TimePoint(
      (completedPhases + 1) * kSearchCaretBlinkInterval);
}

}  // namespace

bool searchCaretOn(wake_schedule::TimePoint now) {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());
  return (elapsed / kSearchCaretBlinkInterval) % 2 == 0;
}

wake_schedule::Deadline nextDeadline(
    wake_schedule::TimePoint now, wake_schedule::TimePoint lastPresented,
    const Activity& activity) {
  wake_schedule::Deadline deadline;
  if (activity.searchCaretVisible) {
    wake_schedule::include(deadline, nextSearchCaretPhase(now));
  }
  if (activity.melodyMonitorVisible) {
    wake_schedule::include(deadline,
                           lastPresented + kMelodyMonitorRefreshInterval);
  }
  if (activity.transportProgressVisible) {
    wake_schedule::include(deadline,
                           lastPresented + kTransportProgressRefreshInterval);
  }
  if (activity.audioPictureInPictureVisible) {
    wake_schedule::include(
        deadline, lastPresented + kAudioPictureInPictureRefreshInterval);
  }
  return deadline;
}

}  // namespace browser_wake_schedule
