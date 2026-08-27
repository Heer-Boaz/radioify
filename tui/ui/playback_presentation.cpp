#include "tui/ui/playback_presentation.h"

#include <cmath>
#include <utility>

namespace {

std::optional<PlaybackTarget> playbackTargetForAudio(
    const std::optional<AudioPlaybackSource>& source) {
  if (!source) return std::nullopt;
  if (source->trackIndex) {
    if (std::optional<PlaybackTarget> trackTarget =
            playbackTrackTarget(source->file, *source->trackIndex)) {
      return trackTarget;
    }
  }
  return playbackFileTarget(source->file);
}

PlaybackControlState controlStateForAudio(const AudioPlaybackSnapshot& audio,
                                          PlaybackTarget target) {
  PlaybackControlState state(std::move(target), false);
  state.positionSec = audio.positionSec;
  if (std::isfinite(audio.durationSec) && audio.durationSec > 0.0) {
    state.durationSec = audio.durationSec;
  }
  state.canPlay = true;
  state.canPause = true;
  state.canStop = true;
  state.canPrevious = true;
  state.canNext = true;
  if (audio.finished) {
    state.status = PlaybackControlStatus::Stopped;
  } else if (audio.paused) {
    state.status = PlaybackControlStatus::Paused;
  } else {
    state.status = PlaybackControlStatus::Playing;
  }
  return state;
}

}  // namespace

PlaybackPresentationModel playbackPresentationModel(
    AudioPlaybackSnapshot audio,
    std::optional<PlaybackControlState> videoControl,
    std::optional<PlaybackPresentationState> videoPresentation) {
  PlaybackPresentationModel model;
  model.audio = std::move(audio);
  model.audioTarget = playbackTargetForAudio(model.audio.source);

  if (videoControl) {
    model.currentTarget = videoControl->target;
    model.control = std::move(videoControl);
    model.videoPresentation = std::move(videoPresentation);
    return model;
  }

  model.currentTarget = model.audioTarget;
  if (model.audioTarget) {
    model.control = controlStateForAudio(model.audio, *model.audioTarget);
  }
  return model;
}
