#include "playback/media_processing_actions.h"

namespace playback_media_processing {

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

ActionExecution Actions::execute(
    playback_media_actions::Action action,
    const std::filesystem::path& sourceFile) const {
  ActionExecution result;
  result.recognized = true;
  switch (action) {
    case playback_media_actions::Action::GenerateSubtitles:
      result.accepted = requestSubtitles(sourceFile);
      result.feedback = result.accepted
                            ? "Generating subtitles (F8 to cancel)"
                            : "Could not start subtitle generation";
      return result;
    case playback_media_actions::Action::CancelSubtitleGeneration:
      result.accepted = requestSubtitleCancellation();
      result.feedback = result.accepted
                            ? "Cancelling subtitle generation"
                            : "Could not cancel subtitle generation";
      return result;
    case playback_media_actions::Action::SeparateAudio:
      result.accepted = requestAudioSeparation(sourceFile);
      result.feedback = result.accepted
                            ? "Separating audio (F8 to cancel)"
                            : "Could not start audio separation";
      return result;
    case playback_media_actions::Action::CancelAudioSeparation:
      result.accepted = requestAudioSeparationCancellation();
      result.feedback = result.accepted
                            ? "Cancelling audio separation"
                            : "Could not cancel audio separation";
      return result;
    case playback_media_actions::Action::Play:
    case playback_media_actions::Action::BrowseTracks:
    case playback_media_actions::Action::EditVideo:
    case playback_media_actions::Action::AnalyzeAudio:
    case playback_media_actions::Action::SplitLoop:
      result.recognized = false;
      return result;
  }
  result.recognized = false;
  return result;
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
