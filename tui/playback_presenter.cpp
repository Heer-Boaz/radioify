#include "tui/playback_presenter.h"

#include <utility>

#include "audio/audioplayback.h"
#include "tui/media_coordinator.h"

PlaybackPresentationModel TuiPlaybackPresenter::model() const {
  AudioPlaybackSnapshot audio = audioPlayback_.snapshot();
  std::optional<TuiMediaCoordinator::VideoSnapshot> video =
      coordinator_.videoSnapshot();
  if (!video) {
    return playbackPresentationModel(std::move(audio), std::nullopt,
                                     std::nullopt);
  }
  return playbackPresentationModel(std::move(audio),
                                   std::move(video->control),
                                   std::move(video->presentation));
}
