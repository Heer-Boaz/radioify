#include "app/playback_route.h"

#include "playback/target_kind.h"
#include "playback/target_resolver.h"

namespace playback_route {
namespace {

bool isVisualTargetKind(PlaybackTargetKind kind) {
  return kind == PlaybackTargetKind::Image || kind == PlaybackTargetKind::Video;
}

PlaybackSessionContinuationState videoContinuation(
    const WindowPlacementState* sourcePlacement,
    PlaybackPresentationState presentation) {
  PlaybackSessionContinuationState state;
  state.hasPresentation = true;
  state.presentation = presentation;
  if (sourcePlacement) {
    state.windowPlacement = *sourcePlacement;
  }
  return state;
}

}  // namespace

Route resolveTarget(
    const PlaybackTarget& target, const WindowPlacementState* sourcePlacement,
    std::optional<PlaybackPresentationState> videoPresentation) {
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
    std::optional<PlaybackPresentationState> videoPresentation) {
  std::optional<PlaybackTarget> target =
      playback_target_resolver::resolveDroppedTarget(files);
  if (!target) {
    return std::nullopt;
  }
  return resolveTarget(*target, sourcePlacement, videoPresentation);
}

}  // namespace playback_route
