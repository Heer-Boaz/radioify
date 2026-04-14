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
