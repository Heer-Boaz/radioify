#pragma once

#include <cstdint>
#include <filesystem>

#include "playback/session/open_outcome.h"

namespace tui_media_activation {

struct DecisionId {
  std::uint64_t value = 0;

  constexpr explicit operator bool() const { return value != 0; }
  friend constexpr bool operator==(DecisionId left, DecisionId right) {
    return left.value == right.value;
  }
  friend constexpr bool operator!=(DecisionId left, DecisionId right) {
    return !(left == right);
  }
};

struct AudioFallbackRequest {
  DecisionId id;
  std::filesystem::path file;
  playback_session::Problem reason;
};

} // namespace tui_media_activation
