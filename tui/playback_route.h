#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "playback/session/state.h"
#include "playback_target.h"

namespace playback_route {

enum class Surface {
  Browser,
  AudioPictureInPicture,
  VideoPresentation,
};

enum class AudioPictureInPicturePlan {
  Keep,
  Close,
};

struct Route {
  PlaybackTarget target;
  AudioPictureInPicturePlan audioPictureInPicture =
      AudioPictureInPicturePlan::Keep;
  std::optional<PlaybackSessionContinuationState> videoContinuation;
};

Route resolveTarget(
    const PlaybackTarget& target, Surface surface,
    const WindowPlacementState* sourcePlacement = nullptr,
    bool preferTextGridVideoPictureInPicture = true);

std::optional<Route> resolveDroppedTarget(
    const std::vector<std::filesystem::path>& files, Surface surface,
    const WindowPlacementState* sourcePlacement = nullptr,
    bool preferTextGridVideoPictureInPicture = true);

}  // namespace playback_route
