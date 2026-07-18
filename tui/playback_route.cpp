#include "playback_route.h"

#include "playback_target_kind.h"
#include "playback_target_resolver.h"

namespace playback_route {
namespace {

bool isVisualTargetKind(PlaybackTargetKind kind) {
  return kind == PlaybackTargetKind::Image || kind == PlaybackTargetKind::Video;
}

PlaybackSessionContinuationState videoContinuation(
    const WindowPlacementState* sourcePlacement,
    PlaybackWindowPresentationRequest presentation) {
  PlaybackSessionContinuationState state;
  state.hasLayout = true;
  state.layout = PlaybackLayout::Window;
  state.asciiRenderingEnabled = presentation.textGrid;
  if (sourcePlacement) {
    state.windowPlacement = *sourcePlacement;
  }
  state.windowPlacement.fullscreenActive =
      presentation.target == PlaybackPresentationMode::Fullscreen;
  state.windowPlacement.pictureInPictureActive =
      presentation.target == PlaybackPresentationMode::PictureInPicture;
  state.windowPlacement.pictureInPictureRestoreFullscreen = false;
  state.windowPlacement.textGridPresentationEnabled = presentation.textGrid;
  state.windowPlacement.pictureInPictureStartedFromTerminal = false;
  return state;
}

}  // namespace

Route resolveTarget(const PlaybackTarget& target,
                    const WindowPlacementState* sourcePlacement,
                    std::optional<PlaybackWindowPresentationRequest>
                        videoPresentation) {
  Route route;
  route.target = target;
  const PlaybackTargetKind targetKind = classifyPlaybackTarget(route.target);

  if (targetKind == PlaybackTargetKind::Video && videoPresentation) {
    route.videoContinuation =
        videoContinuation(sourcePlacement, *videoPresentation);
  }

  if (isVisualTargetKind(targetKind)) {
    route.audioPictureInPicture = AudioPictureInPicturePlan::Close;
  }

  return route;
}

std::optional<Route> resolveDroppedTarget(
    const std::vector<std::filesystem::path>& files,
    const WindowPlacementState* sourcePlacement,
    std::optional<PlaybackWindowPresentationRequest> videoPresentation) {
  std::optional<PlaybackTarget> target =
      playback_target_resolver::resolveDroppedTarget(files);
  if (!target) {
    return std::nullopt;
  }
  return resolveTarget(*target, sourcePlacement, videoPresentation);
}

}  // namespace playback_route
