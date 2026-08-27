#include "playback/media_processing_actions.h"

namespace playback_media_processing {

namespace {

ActionResult actionResult(bool accepted, const char* acceptedFeedback,
                          const char* rejectedFeedback) {
  return {accepted, accepted ? acceptedFeedback : rejectedFeedback};
}

}  // namespace

std::optional<ActionResult> Actions::execute(
    playback_media_actions::Action action,
    const std::filesystem::path& sourceFile) const {
  switch (action) {
    case playback_media_actions::Action::GenerateSubtitles:
      return actionResult(service_.requestSubtitles(sourceFile),
                          "Generating subtitles",
                          "Could not start subtitle generation");
    case playback_media_actions::Action::CancelSubtitleGeneration:
      return actionResult(service_.requestSubtitleCancellation(),
                          "Cancelling subtitle generation",
                          "Could not cancel subtitle generation");
    case playback_media_actions::Action::SeparateAudio:
      return actionResult(
          service_.requestAudioSeparation(sourceFile),
          "Separating audio",
          "Could not start audio separation");
    case playback_media_actions::Action::CancelAudioSeparation:
      return actionResult(
          service_.requestAudioSeparationCancellation(),
          "Cancelling audio separation",
          "Could not cancel audio separation");
    case playback_media_actions::Action::Play:
    case playback_media_actions::Action::BrowseTracks:
    case playback_media_actions::Action::EditVideo:
    case playback_media_actions::Action::AnalyzeAudio:
    case playback_media_actions::Action::SplitLoop:
      return std::nullopt;
  }
  return std::nullopt;
}

playback_media_actions::Context Actions::contextForSource(
    const std::filesystem::path& sourceFile) const {
  playback_media_actions::Context context;
  context.mediaKind = playback_media_actions::mediaKindForSource(sourceFile);
  const SourceState state = service_.sourceStateFor(sourceFile);
  context.backgroundTaskRunning = state.backgroundTaskRunning;
  context.subtitleGenerationRunningForSource =
      state.subtitleGenerationRunning;
  context.hasGeneratedSubtitles = state.hasGeneratedSubtitles;
  context.canSeparateAudio = state.audioSeparationAvailable;
  context.audioSeparationRunningForSource = state.audioSeparationRunning;
  context.hasSeparatedAudio = state.separatedAudioExists;
  return context;
}

}  // namespace playback_media_processing
