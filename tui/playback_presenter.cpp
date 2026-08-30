#include "tui/playback_presenter.h"

#include <utility>

#include "tui/media_coordinator.h"

PlaybackPresentationModel TuiPlaybackPresenter::model() const {
  TuiMediaCoordinator::PlaybackSnapshot playback =
      coordinator_.playbackSnapshot();
  std::optional<PlaybackControlState> videoControl;
  std::optional<PlaybackPresentationState> videoPresentation;
  if (playback.video) {
    videoControl = std::move(playback.video->control);
    videoPresentation = std::move(playback.video->presentation);
  }
  return playbackPresentationModel(
      std::move(playback.audio), playback.controlSession,
      std::move(videoControl), std::move(videoPresentation));
}
