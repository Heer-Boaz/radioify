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

// Command/query boundary between input policy and the playback session. The
// concrete player and the session state machine stay behind this owner.
class Transport {
 public:
  virtual ~Transport() = default;

  virtual TransportSnapshot snapshot() const = 0;
  virtual bool seekTo(int64_t targetUs) = 0;
  virtual bool seekBy(int64_t deltaUs) = 0;
  virtual void setPaused(bool paused) = 0;
  virtual bool requestFrameStep(
      playback_video_frame_step::Direction direction) = 0;
  virtual bool cycleAudioTrack() = 0;
};

}  // namespace playback_session_input
