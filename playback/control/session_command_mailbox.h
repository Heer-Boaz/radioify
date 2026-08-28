#pragma once

#include <deque>
#include <mutex>

#include "playback/control/command.h"

// Thread-safe boundary between process-wide operating-system callbacks and a
// particular playback activation. Changing or ending the active session
// atomically invalidates every queued command for the previous one.
class PlaybackControlSessionCommandMailbox {
 public:
  void activate(PlaybackControlSessionId session);
  void deactivate();
  void publish(PlaybackControlCommand command);
  bool poll(PlaybackControlCommandEvent* out);

 private:
  std::mutex mutex_;
  PlaybackControlSessionId activeSession_;
  std::deque<PlaybackControlCommandEvent> pending_;
};
