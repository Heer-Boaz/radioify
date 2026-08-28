#pragma once

#include <cstdint>

enum class PlaybackControlCommand : uint8_t {
  Play,
  Pause,
  TogglePause,
  Stop,
  Previous,
  Next,
};

// Identifies one accepted playback activation. A path is deliberately not a
// session identity: reopening the same file must invalidate commands emitted
// for the previous playback instance.
struct PlaybackControlSessionId {
  uint64_t value = 0;

  constexpr bool valid() const noexcept { return value != 0; }
};

constexpr bool operator==(PlaybackControlSessionId lhs,
                          PlaybackControlSessionId rhs) noexcept {
  return lhs.value == rhs.value;
}

constexpr bool operator!=(PlaybackControlSessionId lhs,
                          PlaybackControlSessionId rhs) noexcept {
  return !(lhs == rhs);
}

struct PlaybackControlCommandEvent {
  PlaybackControlSessionId session;
  PlaybackControlCommand command = PlaybackControlCommand::TogglePause;
};
