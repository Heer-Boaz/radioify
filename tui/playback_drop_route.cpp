#include "playback_drop_route.h"

#include "media_formats.h"
#include "playback_target_resolver.h"

namespace playback_drop_route {
namespace {

DroppedMediaKind classify(const PlaybackTarget& target) {
  if (target.trackIndex >= 0) {
    return DroppedMediaKind::Audio;
  }
  if (isSupportedImageExt(target.file)) {
    return DroppedMediaKind::Image;
  }
  if (isSupportedVideoExt(target.file)) {
    return DroppedMediaKind::Video;
  }
  return DroppedMediaKind::Audio;
}

PlaybackSessionContinuationState videoMiniPlayerContinuation(
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

std::optional<DropRoute> resolve(
    const std::vector<std::filesystem::path>& files, DropSurface surface,
    const WindowPlacementState* sourcePlacement,
    bool preferTextGridVideoMiniPlayer) {
  std::optional<PlaybackTarget> target =
      playback_target_resolver::resolveDroppedTarget(files);
  if (!target) {
    return std::nullopt;
  }

  DropRoute route;
  route.target = *target;
  route.kind = classify(route.target);

  switch (surface) {
    case DropSurface::AudioMiniPlayer:
      if (route.kind == DroppedMediaKind::Video) {
        route.closeAudioMiniPlayer = true;
        route.videoContinuation = videoMiniPlayerContinuation(
            sourcePlacement, preferTextGridVideoMiniPlayer);
      }
      break;
    case DropSurface::VideoPresentation:
      if (route.kind == DroppedMediaKind::Audio) {
        route.openAudioMiniPlayer = true;
      }
      break;
    case DropSurface::Browser:
      break;
  }

  return route;
}

}  // namespace playback_drop_route
