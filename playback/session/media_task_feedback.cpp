#include "media_task_feedback.h"

#include "core/runtime_helpers.h"

namespace playback_session {

std::string mediaTaskFeedback(
    const playback_media_processing::Completion& completion) {
  using Operation = playback_media_processing::Operation;
  using Outcome = playback_media_processing::Outcome;
  if (completion.outcome == Outcome::Cancelled) {
    switch (completion.operation) {
      case Operation::SubtitleGeneration:
        return "Subtitle generation cancelled.";
      case Operation::AudioSeparation:
        return "Audio separation cancelled.";
      case Operation::AudioExport:
        return "Audio export cancelled.";
      case Operation::TranscriptTextExport:
        return "Transcript export cancelled.";
    }
  }
  if (completion.outcome == Outcome::Failed) {
    if (!completion.detail.empty()) return completion.detail;
    switch (completion.operation) {
      case Operation::SubtitleGeneration:
        return "Subtitle generation failed.";
      case Operation::AudioSeparation:
        return "Audio separation failed.";
      case Operation::AudioExport:
        return "Audio export failed.";
      case Operation::TranscriptTextExport:
        return "Transcript export failed.";
    }
  }
  if (completion.operation == Operation::AudioSeparation) {
    return "Audio stems ready: dialogue, music and effects.";
  }
  const std::string filename =
      toUtf8String(completion.outputFile.filename());
  switch (completion.operation) {
    case Operation::SubtitleGeneration:
      return filename.empty() ? "Subtitles ready."
                              : "Subtitles ready: " + filename;
    case Operation::AudioExport:
      return filename.empty() ? "Audio export ready."
                              : "Audio export ready: " + filename;
    case Operation::TranscriptTextExport:
      return filename.empty() ? "Transcript export ready."
                              : "Transcript export ready: " + filename;
    case Operation::AudioSeparation:
      break;
  }
  return {};
}

}  // namespace playback_session
