#pragma once

#include <cstdint>

namespace playback_media_processing {

enum class AudioSeparationAvailability : std::uint8_t {
  Unavailable,
  SetupRequired,
  Ready,
};

}  // namespace playback_media_processing
