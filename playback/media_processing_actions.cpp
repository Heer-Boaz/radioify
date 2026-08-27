#include "playback/media_processing_actions.h"

#include "playback/media_action_context.h"

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
    case playback_media_actions::Action::ExportAudio:
      return actionResult(service_.requestAudioExport(sourceFile),
                          "Exporting audio",
                          "Could not start audio export");
    case playback_media_actions::Action::ExportTranscriptText:
      return actionResult(service_.requestTranscriptTextExport(sourceFile),
                          "Exporting transcript",
                          "Could not start transcript export");
    case playback_media_actions::Action::CancelMediaExport:
      return actionResult(service_.requestMediaExportCancellation(),
                          "Cancelling export",
                          "Could not cancel export");
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
  context.canExportAudio = state.audioExportAvailable;
  context.audioExportRunningForSource = state.audioExportRunning;
  context.canExportTranscriptText = state.transcriptTextExportAvailable;
  context.transcriptTextExportRunningForSource =
      state.transcriptTextExportRunning;
  return context;
}

}  // namespace playback_media_processing
