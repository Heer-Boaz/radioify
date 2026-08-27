#pragma once

#include "core/wake_deadline.h"

namespace browser_wake_schedule {

struct Activity {
  bool searchCaretVisible = false;
  bool melodyMonitorVisible = false;
  bool transportProgressVisible = false;
  bool audioPictureInPictureVisible = false;
};

bool searchCaretOn(wake_schedule::TimePoint now);

wake_schedule::Deadline nextDeadline(
    wake_schedule::TimePoint now, wake_schedule::TimePoint lastPresented,
    const Activity& activity);

}  // namespace browser_wake_schedule
