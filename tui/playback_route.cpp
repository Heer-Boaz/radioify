#include "playback_route.h"

#include "playback_target_kind.h"
#include "playback_target_resolver.h"

namespace playback_route {
namespace {

bool isVisualTargetKind(PlaybackTargetKind kind) {
  return kind == PlaybackTargetKind::Image || kind == PlaybackTargetKind::Video;
}

PlaybackSessionContinuationState videoPictureInPictureContinuation(
    const WindowPlacementState* sourcePlacement, bool textGrid) {
  PlaybackSessionContinuationState state;
  state.hasLayout = true;
  state.layout = PlaybackLayout::Window;
  if (sourcePlacement) {
    state.windowPlacement = *sourcePlacement;
  }
  state.windowPlacement.fullscreenActive = false;
  state.windowPlacement.pictureInPictureActive = true;
  state.windowPlacement.pictureInPictureRestoreFullscreen = false;
  state.windowPlacement.textGridPresentationEnabled = textGrid;
  state.windowPlacement.pictureInPictureStartedFromTerminal = false;
  return state;
}

}  // namespace

Route resolveTarget(const PlaybackTarget& target, Surface surface,
                    const WindowPlacementState* sourcePlacement,
                    bool preferTextGridVideoPictureInPicture) {
  Route route;
  route.target = target;
  const PlaybackTargetKind targetKind = classifyPlaybackTarget(route.target);

  switch (surface) {
    case Surface::Browser:
      if (isVisualTargetKind(targetKind)) {
        route.audioPictureInPicture = AudioPictureInPicturePlan::Close;
      }
      break;
    case Surface::AudioPictureInPicture:
      if (targetKind == PlaybackTargetKind::Video) {
        route.videoContinuation = videoPictureInPictureContinuation(
            sourcePlacement, preferTextGridVideoPictureInPicture);
      }
      if (isVisualTargetKind(targetKind)) {
        route.audioPictureInPicture = AudioPictureInPicturePlan::Close;
      }
      break;
    case Surface::VideoPresentation:
      if (isVisualTargetKind(targetKind)) {
        route.audioPictureInPicture = AudioPictureInPicturePlan::Close;
      }
      break;
  }

  return route;
}

std::optional<Route> resolveDroppedTarget(
    const std::vector<std::filesystem::path>& files, Surface surface,
    const WindowPlacementState* sourcePlacement,
    bool preferTextGridVideoPictureInPicture) {
  std::optional<PlaybackTarget> target =
      playback_target_resolver::resolveDroppedTarget(files);
  if (!target) {
    return std::nullopt;
  }
  return resolveTarget(*target, surface, sourcePlacement,
                       preferTextGridVideoPictureInPicture);
}

}  // namespace playback_route
