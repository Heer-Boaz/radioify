#include "playback/media_processing_presentation.h"

#include <algorithm>
#include <cmath>

#include "core/runtime_helpers.h"

namespace playback_media_processing {
namespace {

std::string activityTitle(const Activity& activity) {
  if (activity.operation == Operation::AudioSeparation) {
    switch (activity.scheduling) {
      case SchedulingState::Running:
        break;
      case SchedulingState::Suspending:
        return "Pausing audio separation";
      case SchedulingState::Suspended:
        return "Audio separation paused";
    }
  }
  switch (activity.operation) {
    case Operation::MelodyAnalysis:
      return activity.cancelling ? "Cancelling melody analysis"
                                 : "Analyzing melody";
    case Operation::LoopSplit:
      return activity.cancelling ? "Cancelling loop split"
                                 : "Splitting loop";
    case Operation::SubtitleGeneration:
      return activity.cancelling ? "Cancelling subtitles"
                                 : "Generating subtitles";
    case Operation::AudioSeparationSetup:
      return activity.cancelling ? "Cancelling audio separation setup"
                                 : "Setting up audio separation";
    case Operation::AudioSeparation:
      return activity.cancelling ? "Cancelling audio separation"
                                 : "Separating audio";
    case Operation::AudioExport:
      return activity.cancelling ? "Cancelling audio export"
                                 : "Exporting audio";
    case Operation::TranscriptTextExport:
      return activity.cancelling ? "Cancelling transcript export"
                                 : "Exporting transcript";
  }
  return "Processing media";
}

}  // namespace

ActivityPresentation presentActivity(const Activity& activity) {
  ActivityPresentation presentation;
  presentation.title = activityTitle(activity);
  presentation.detail = activity.phase;
  if (activity.progress && std::isfinite(*activity.progress)) {
    presentation.progress = std::clamp(*activity.progress, 0.0f, 1.0f);
  }
  return presentation;
}

std::string activityStatusLine(const Activity& activity) {
  const ActivityPresentation presentation = presentActivity(activity);
  std::string status = presentation.title;
  if (!activity.sourceFile.empty()) {
    const std::filesystem::path filename = activity.sourceFile.filename();
    const std::string source =
        toUtf8String(filename.empty() ? activity.sourceFile : filename);
    if (!source.empty()) status += " - " + source;
  }
  if (presentation.progress) {
    status += " " + std::to_string(static_cast<int>(
                         std::lround(*presentation.progress * 100.0f))) +
              "%";
  }
  if (!presentation.detail.empty()) {
    status += " - " + presentation.detail;
  }
  return status;
}

}  // namespace playback_media_processing
