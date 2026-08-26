#include "app/media_processing_playback_service.h"

#include <utility>

#include "core/path_identity.h"

namespace media_processing {

PlaybackService::PlaybackService(Coordinator& coordinator,
                                 StateChanged stateChanged)
    : coordinator_(coordinator), stateChanged_(std::move(stateChanged)) {}

bool PlaybackService::busy() const { return coordinator_.running(); }

playback_media_processing::SourceState PlaybackService::sourceStateFor(
    const std::filesystem::path& sourceFile) const {
  playback_media_processing::SourceState state;
  const std::optional<TaskActivity> activity = coordinator_.activity();
  state.backgroundTaskRunning = activity.has_value();
  if (activity && samePath(activity->sourceFile, sourceFile)) {
    state.subtitleGenerationRunning =
        activity->kind == TaskKind::SubtitleGeneration;
    state.audioSeparationRunning =
        activity->kind == TaskKind::AudioSeparation;
  }
  state.audioSeparationAvailable =
      coordinator_.audioSeparationAvailableFor(sourceFile);
  state.separatedAudioExists =
      coordinator_.hasSeparatedAudioFor(sourceFile);
  return state;
}

bool PlaybackService::requestSubtitles(
    const std::filesystem::path& sourceFile) {
  return publishAccepted(
      coordinator_.tryStartSubtitleGeneration(sourceFile));
}

bool PlaybackService::requestSubtitleCancellation() {
  return publishAccepted(coordinator_.cancelSubtitleGeneration());
}

bool PlaybackService::requestAudioSeparation(
    const std::filesystem::path& sourceFile) {
  return publishAccepted(coordinator_.tryStartAudioSeparation(sourceFile));
}

bool PlaybackService::requestAudioSeparationCancellation() {
  return publishAccepted(coordinator_.cancelAudioSeparation());
}

bool PlaybackService::requestActiveCancellation() {
  return publishAccepted(coordinator_.cancelActive());
}

bool PlaybackService::publishAccepted(bool accepted) {
  if (accepted && stateChanged_) stateChanged_();
  return accepted;
}

}  // namespace media_processing
