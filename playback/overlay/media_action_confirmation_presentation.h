#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace playback_overlay {

enum class MediaActionConfirmationSelection : std::uint8_t {
  Primary,
  Secondary,
};

// Presentation-only projection of a session-owned decision. Task identity,
// source paths and executable capabilities never reach render backends.
struct MediaActionConfirmationDialog {
  std::string title;
  std::vector<std::string> text;
  std::string primaryLabel;
  std::string secondaryLabel;
  MediaActionConfirmationSelection selected =
      MediaActionConfirmationSelection::Secondary;
};

}  // namespace playback_overlay
