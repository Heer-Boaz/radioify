#include "playback/media_processing_actions.h"

#include <utility>

#include "core/runtime_helpers.h"
#include "playback/media_action_context.h"

namespace playback_media_processing {

namespace {

struct ActionText {
  const char* accepted = "Media processing started";
  const char* rejected = "Media processing could not start";
};

ActionText actionText(playback_media_actions::Action action) {
  using Action = playback_media_actions::Action;
  switch (action) {
    case Action::GenerateSubtitles:
      return {"Generating subtitles",
              "Subtitle generation could not start"};
    case Action::CancelSubtitleGeneration:
      return {"Cancelling subtitle generation",
              "Subtitle generation could not be cancelled"};
    case Action::ExportAudio:
      return {"Exporting audio", "Audio export could not start"};
    case Action::ExportTranscriptText:
      return {"Exporting transcript", "Transcript export could not start"};
    case Action::CancelMediaExport:
      return {"Cancelling export", "Media export could not be cancelled"};
    case Action::SeparateAudio:
      return {"Separating audio", "Audio separation could not start"};
    case Action::CancelAudioSeparation:
      return {"Cancelling audio separation",
              "Audio separation could not be cancelled"};
    case Action::AnalyzeAudio:
      return {"Analyzing melody", "Melody analysis could not start"};
    case Action::SplitLoop:
      return {"Splitting loop", "Loop splitting could not start"};
    case Action::Play:
    case Action::BrowseTracks:
    case Action::EditVideo:
      break;
  }
  return {};
}

std::string displayName(const std::filesystem::path& sourceFile) {
  const std::filesystem::path filename = sourceFile.filename();
  return toUtf8String(filename.empty() ? sourceFile : filename);
}

std::string rejectionReason(const RequestError& error) {
  if (error.failure == RequestFailure::Busy && error.blockingOperation) {
    std::string reason = operationDisplayName(*error.blockingOperation);
    reason += " is already running";
    const std::string blockingSource = displayName(error.blockingSourceFile);
    if (!blockingSource.empty()) {
      reason += " for \"" + blockingSource + "\"";
    }
    return reason;
  }
  if (!error.detail.empty()) return error.detail;

  switch (error.failure) {
    case RequestFailure::InvalidSource:
      return "no source file was supplied";
    case RequestFailure::UnsupportedSource:
      return "the selected file type is not supported";
    case RequestFailure::InvalidSelection:
      return "the selected media track is invalid";
    case RequestFailure::InvalidDestination:
      return "no valid output path could be created";
    case RequestFailure::BackendUnavailable:
      return "the required processing backend is unavailable";
    case RequestFailure::ManagedArtifact:
      return "select the original media file instead of a generated audio "
             "stem";
    case RequestFailure::MissingTranscript:
      return "generate subtitles for this video first";
    case RequestFailure::Busy:
      return "another media-processing task is already running";
    case RequestFailure::CompletionPending:
      return "the previous media-processing result is still being finalized; "
             "try again";
    case RequestFailure::NotRunning:
      return "there is no matching task to cancel";
    case RequestFailure::AlreadyCancelling:
      return "the task is already being cancelled";
    case RequestFailure::InternalError:
      return "the processing worker rejected a validated request";
  }
  return "the request was rejected";
}

}  // namespace

ActionResult makeActionResult(playback_media_actions::Action action,
                              const std::filesystem::path& sourceFile,
                              const RequestResult& requestResult) {
  const ActionText text = actionText(action);
  if (requestResult.wasAccepted()) {
    return {true, text.accepted, std::nullopt};
  }

  ActionResult result;
  result.accepted = false;
  result.error = requestResult.error();
  result.feedback = text.rejected;
  if (result.error) {
    result.feedback += ": " + rejectionReason(*result.error);
  }
  const std::string source = displayName(sourceFile);
  if (!source.empty()) {
    result.feedback += ". Source: \"" + source + "\"";
  }
  result.feedback += ".";
  return result;
}

std::optional<ActionResult> Actions::execute(
    playback_media_actions::Action action,
    const std::filesystem::path& sourceFile) const {
  switch (action) {
    case playback_media_actions::Action::GenerateSubtitles:
      return makeActionResult(action, sourceFile,
                              service_.requestSubtitles(sourceFile));
    case playback_media_actions::Action::CancelSubtitleGeneration:
      return makeActionResult(action, sourceFile,
                              service_.requestSubtitleCancellation());
    case playback_media_actions::Action::ExportAudio:
      return makeActionResult(action, sourceFile,
                              service_.requestAudioExport(sourceFile));
    case playback_media_actions::Action::ExportTranscriptText:
      return makeActionResult(
          action, sourceFile,
          service_.requestTranscriptTextExport(sourceFile));
    case playback_media_actions::Action::CancelMediaExport:
      return makeActionResult(action, sourceFile,
                              service_.requestMediaExportCancellation());
    case playback_media_actions::Action::SeparateAudio:
      return makeActionResult(action, sourceFile,
                              service_.requestAudioSeparation(sourceFile));
    case playback_media_actions::Action::CancelAudioSeparation:
      return makeActionResult(
          action, sourceFile,
          service_.requestAudioSeparationCancellation());
    case playback_media_actions::Action::Play:
    case playback_media_actions::Action::BrowseTracks:
    case playback_media_actions::Action::EditVideo:
    case playback_media_actions::Action::AnalyzeAudio:
    case playback_media_actions::Action::SplitLoop:
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<CancellationRequest> Actions::prepareCancellation(
    playback_media_actions::Action action,
    const std::filesystem::path& sourceFile) const {
  return playback_media_processing::prepareCancellation(
      action, sourceFile, service_.sourceStateFor(sourceFile));
}

playback_media_actions::Context Actions::contextForSource(
    const std::filesystem::path& sourceFile) const {
  playback_media_actions::Context context;
  context.mediaKind = playback_media_actions::mediaKindForSource(sourceFile);
  const SourceState state = service_.sourceStateFor(sourceFile);
  context.backgroundTaskRunning = state.backgroundTaskRunning;
  context.activeTaskCancellable = state.activeTaskCancellable;
  context.canGenerateSubtitles = state.subtitleGenerationAvailable;
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
