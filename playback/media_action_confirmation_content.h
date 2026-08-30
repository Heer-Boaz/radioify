#pragma once

#include <string>
#include <vector>

#include "playback/media_processing_service.h"

namespace playback_media_confirmation {

// Cross-surface presentation contract for one explicit decision. Browser and
// playback surfaces adapt this value to their renderers instead of owning
// separate wording or button order.
struct Content {
  std::string title;
  std::vector<std::string> text;
  std::string primaryLabel;
  std::string secondaryLabel;
};

Content cancellationContent(
    playback_media_processing::Operation operation,
    std::string sourceName);
Content audioSeparationSetupContent();

}  // namespace playback_media_confirmation
