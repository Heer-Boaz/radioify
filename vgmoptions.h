#pragma once

#include <cstdint>

enum class VgmTempoMode : uint8_t {
  Normal = 0,
  Pal50 = 1,
};

struct VgmPlaybackOptions {
  VgmTempoMode tempoMode = VgmTempoMode::Normal;
};

enum class VgmOptionId : uint8_t {
  TempoMode = 0,
};
