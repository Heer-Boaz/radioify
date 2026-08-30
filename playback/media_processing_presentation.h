#pragma once

#include <optional>
#include <string>

#include "playback/media_processing_service.h"

namespace playback_media_processing {

struct ActivityPresentation {
  std::string title;
  std::string detail;
  std::optional<float> progress;
};

// Shared copy and progress projection for every playback surface. Renderers
// may lay it out differently, but browser and player must describe the same
// application task consistently.
ActivityPresentation presentActivity(const Activity& activity);
std::string activityStatusLine(const Activity& activity);

}  // namespace playback_media_processing
