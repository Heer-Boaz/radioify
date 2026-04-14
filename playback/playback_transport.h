#pragma once

#include <cstdint>

enum class PlaybackTransportCommand : uint8_t {
  None,
  Previous,
  Next,
};
