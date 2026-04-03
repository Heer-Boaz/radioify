#pragma once

#include <cstdint>

enum class PlaybackSessionState : uint8_t {
  Active,
  Paused,
  Ended,
  Exiting,
};

enum class WindowThreadState : uint8_t {
  Disabled,
  Enabled,
  Stopping,
};
