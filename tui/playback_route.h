#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "playback/session/presentation_policy.h"
#include "playback/session/state.h"
#include "playback_target.h"

namespace playback_route {

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
    const PlaybackTarget& target,
    const WindowPlacementState* sourcePlacement = nullptr,
    std::optional<PlaybackWindowPresentationRequest> videoPresentation =
        std::nullopt);

std::optional<Route> resolveDroppedTarget(
    const std::vector<std::filesystem::path>& files,
    const WindowPlacementState* sourcePlacement = nullptr,
    std::optional<PlaybackWindowPresentationRequest> videoPresentation =
        std::nullopt);

}  // namespace playback_route
