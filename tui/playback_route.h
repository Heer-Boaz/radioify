#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "app/playback_controller.h"
#include "playback/session/presentation_policy.h"
#include "playback/session/state.h"
#include "playback/target.h"

namespace playback_route {

enum class AudioPictureInPicturePlan {
  Keep,
  Close,
};

struct Route {
  PlaybackTarget target;
  PlaybackSessionIntent sessionIntent = PlaybackSessionIntent::View;
  AudioPictureInPicturePlan audioPictureInPicture =
      AudioPictureInPicturePlan::Keep;
  std::optional<PlaybackSessionContinuationState> videoContinuation;
};

struct Request {
  Route route;
  playback_controller::Transition transition;
};

Request start(Route route, playback_controller::Source source);
Request continueWith(Route route);

Route resolveTarget(const PlaybackTarget& target,
                    const WindowPlacementState* sourcePlacement = nullptr,
                    std::optional<PlaybackWindowPresentationRequest>
                        videoPresentation = std::nullopt);

std::optional<Route> resolveDroppedTarget(
    const std::vector<std::filesystem::path>& files,
    const WindowPlacementState* sourcePlacement = nullptr,
    std::optional<PlaybackWindowPresentationRequest> videoPresentation =
        std::nullopt);

}  // namespace playback_route
