#include "playback/media_processing_actions.h"

namespace playback_media_processing {

bool Actions::busy() const { return service_ && service_->busy(); }

bool Actions::requestSubtitles(
    const std::filesystem::path& sourceFile) const {
  return service_ && service_->requestSubtitles(sourceFile);
}

bool Actions::requestSubtitleCancellation() const {
  return service_ && service_->requestSubtitleCancellation();
}

bool Actions::requestAudioSeparation(
    const std::filesystem::path& sourceFile) const {
  return service_ && service_->requestAudioSeparation(sourceFile);
}

bool Actions::requestAudioSeparationCancellation() const {
  return service_ && service_->requestAudioSeparationCancellation();
}

void Actions::applySourceState(
    const std::filesystem::path& sourceFile,
    playback_media_actions::Context* context) const {
  if (!context) return;
  const SourceState state =
      service_ ? service_->sourceStateFor(sourceFile) : SourceState{};
  context->backgroundTaskRunning = state.backgroundTaskRunning;
  context->subtitleGenerationRunningForSource =
      state.subtitleGenerationRunning;
  context->canSeparateAudio = state.audioSeparationAvailable;
  context->audioSeparationRunningForSource = state.audioSeparationRunning;
  context->hasSeparatedAudio = state.separatedAudioExists;
}

}  // namespace playback_media_processing
