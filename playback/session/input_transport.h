#pragma once

#include <cstdint>

#include "playback/session/state.h"
#include "playback/video/frame_step.h"

namespace playback_session_input {

struct TransportSnapshot {
  PlaybackSessionState state = PlaybackSessionState::Active;
  bool audioAvailable = false;
  bool ended = false;
  bool seekPending = false;
  int64_t durationUs = 0;
  int64_t positionUs = 0;
  uint64_t latestSeekRequestGeneration = 0;
};

}  // namespace playback_session_input
