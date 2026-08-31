#include "media_task_feedback.h"

#include <utility>

#include "core/runtime_helpers.h"

namespace playback_session {

std::string mediaTaskFeedback(
    const playback_media_processing::Completion& completion,
    const std::filesystem::path& identifySource) {
  using Operation = playback_media_processing::Operation;
  using Outcome = playback_media_processing::Outcome;
  const auto identify = [&](std::string message) {
    if (identifySource.empty()) return message;
    const std::filesystem::path filename = identifySource.filename();
    const std::string source =
        toUtf8String(filename.empty() ? identifySource : filename);
    return source.empty() ? message : source + ": " + std::move(message);
  };
  if (completion.outcome == Outcome::Cancelled) {
    switch (completion.operation) {
      case Operation::MelodyAnalysis:
        return identify("Melody analysis cancelled.");
      case Operation::LoopSplit:
        return identify("Loop splitting cancelled.");
      case Operation::SubtitleGeneration:
        return identify("Transcript generation cancelled.");
      case Operation::AudioSeparationSetup:
        return identify("Audio separation setup cancelled.");
      case Operation::AudioSeparation:
        return identify("Audio separation cancelled.");
      case Operation::AudioExport:
        return identify("Audio export cancelled.");
      case Operation::TranscriptTextExport:
        return identify("Transcript export cancelled.");
    }
  }
  if (completion.outcome == Outcome::Failed) {
    if (!completion.detail.empty()) return identify(completion.detail);
    switch (completion.operation) {
      case Operation::MelodyAnalysis:
        return identify("Melody analysis failed.");
      case Operation::LoopSplit:
        return identify("Loop splitting failed.");
      case Operation::SubtitleGeneration:
        return identify("Transcript generation failed.");
      case Operation::AudioSeparationSetup:
        return identify("Audio separation setup failed.");
      case Operation::AudioSeparation:
        return identify("Audio separation failed.");
      case Operation::AudioExport:
        return identify("Audio export failed.");
      case Operation::TranscriptTextExport:
        return identify("Transcript export failed.");
    }
  }
  if (completion.operation == Operation::AudioSeparation) {
    return identify("Audio stems ready: dialogue, music and effects.");
  }
  const std::string filename =
      toUtf8String(completion.outputFile.filename());
  switch (completion.operation) {
    case Operation::MelodyAnalysis:
      return identify(filename.empty() ? "Melody analysis complete."
                                       : "Melody analysis ready: " + filename);
    case Operation::LoopSplit:
      return identify(filename.empty() ? "Loop split complete."
                                       : "Loop split ready: " + filename);
    case Operation::SubtitleGeneration:
      return identify(filename.empty() ? "Transcript ready."
                                       : "Transcript ready: " + filename);
    case Operation::AudioSeparationSetup:
      return identify("Audio separation is ready.");
    case Operation::AudioExport:
      return identify(filename.empty() ? "Audio export ready."
                                       : "Audio export ready: " + filename);
    case Operation::TranscriptTextExport:
      return identify(filename.empty() ? "Transcript export ready."
                                       : "Transcript export ready: " + filename);
    case Operation::AudioSeparation:
      break;
  }
  return {};
}

}  // namespace playback_session
