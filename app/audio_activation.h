#pragma once

#include <cstdint>
#include <filesystem>

#include "playback/session/problem.h"

namespace application_playback {

struct AudioFallbackDecisionId {
  std::uint64_t value = 0;

  constexpr explicit operator bool() const noexcept { return value != 0; }
  friend constexpr bool operator==(AudioFallbackDecisionId left,
                                   AudioFallbackDecisionId right) noexcept {
    return left.value == right.value;
  }
  friend constexpr bool operator!=(AudioFallbackDecisionId left,
                                   AudioFallbackDecisionId right) noexcept {
    return !(left == right);
  }
};

struct AudioFallbackRequest {
  AudioFallbackDecisionId id;
  std::filesystem::path file;
  playback_session::Problem reason;
};

struct AudioFallbackRevoked {
  AudioFallbackDecisionId id;
};

}  // namespace application_playback
