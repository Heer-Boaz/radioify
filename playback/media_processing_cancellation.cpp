#include "playback/media_processing_actions.h"

namespace playback_media_processing {
namespace {

std::optional<Operation> fixedCancellationOperation(
    playback_media_actions::Action action) {
  switch (action) {
    case playback_media_actions::Action::CancelSubtitleGeneration:
      return Operation::SubtitleGeneration;
    case playback_media_actions::Action::CancelMediaExport:
      return std::nullopt;
    case playback_media_actions::Action::CancelAudioSeparation:
      return Operation::AudioSeparation;
    case playback_media_actions::Action::Play:
    case playback_media_actions::Action::BrowseTracks:
    case playback_media_actions::Action::EditVideo:
    case playback_media_actions::Action::GenerateSubtitles:
    case playback_media_actions::Action::ExportTranscriptText:
    case playback_media_actions::Action::ExportAudio:
    case playback_media_actions::Action::SeparateAudio:
    case playback_media_actions::Action::AnalyzeAudio:
    case playback_media_actions::Action::SplitLoop:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace

bool isCancellationAction(playback_media_actions::Action action) {
  return action == playback_media_actions::Action::CancelMediaExport ||
         fixedCancellationOperation(action).has_value();
}

bool cancellationTargetsOperation(playback_media_actions::Action action,
                                  Operation operation) {
  if (action == playback_media_actions::Action::CancelMediaExport) {
    return operation == Operation::AudioExport ||
           operation == Operation::TranscriptTextExport;
  }
  const std::optional<Operation> target = fixedCancellationOperation(action);
  return target && *target == operation;
}

std::optional<CancellationRequest> prepareCancellation(
    playback_media_actions::Action action,
    const std::filesystem::path& sourceFile, const SourceState& state) {
  std::optional<Operation> operation;
  switch (action) {
    case playback_media_actions::Action::CancelSubtitleGeneration:
      if (state.subtitleGenerationRunning) {
        operation = Operation::SubtitleGeneration;
      }
      break;
    case playback_media_actions::Action::CancelMediaExport:
      if (state.audioExportRunning != state.transcriptTextExportRunning) {
        operation = state.audioExportRunning ? Operation::AudioExport
                                             : Operation::TranscriptTextExport;
      }
      break;
    case playback_media_actions::Action::CancelAudioSeparation:
      if (state.audioSeparationRunning) {
        operation = Operation::AudioSeparation;
      }
      break;
    case playback_media_actions::Action::Play:
    case playback_media_actions::Action::BrowseTracks:
    case playback_media_actions::Action::EditVideo:
    case playback_media_actions::Action::GenerateSubtitles:
    case playback_media_actions::Action::ExportTranscriptText:
    case playback_media_actions::Action::ExportAudio:
    case playback_media_actions::Action::SeparateAudio:
    case playback_media_actions::Action::AnalyzeAudio:
    case playback_media_actions::Action::SplitLoop:
      break;
  }
  if (!operation || !state.activeTaskCancellable || sourceFile.empty() ||
      !state.activeTaskId ||
      !*state.activeTaskId) {
    return std::nullopt;
  }
  return CancellationRequest{*state.activeTaskId, action, *operation,
                             sourceFile};
}

}  // namespace playback_media_processing
