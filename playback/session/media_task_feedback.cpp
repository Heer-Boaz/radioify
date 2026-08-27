#include "media_task_feedback.h"

#include "core/runtime_helpers.h"

namespace playback_session {

std::string mediaTaskFeedback(
    const playback_media_processing::Completion& completion) {
  using Operation = playback_media_processing::Operation;
  using Outcome = playback_media_processing::Outcome;
  if (completion.outcome == Outcome::Cancelled) {
    return completion.operation == Operation::SubtitleGeneration
               ? "Subtitle generation cancelled."
               : "Audio separation cancelled.";
  }
  if (completion.outcome == Outcome::Failed) {
    if (!completion.detail.empty()) return completion.detail;
    return completion.operation == Operation::SubtitleGeneration
               ? "Subtitle generation failed."
               : "Audio separation failed.";
  }
  if (completion.operation == Operation::AudioSeparation) {
    return "Audio stems ready: dialogue, music and effects.";
  }
  const std::string filename =
      toUtf8String(completion.outputFile.filename());
  return filename.empty() ? "Subtitles ready."
                          : "Subtitles ready: " + filename;
}

}  // namespace playback_session
