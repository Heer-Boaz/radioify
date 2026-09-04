#include "playback/video/chapter/action_catalog.h"

#include <algorithm>
#include <cmath>

namespace playback_video_chapters {
namespace {

std::string runningLabel(const Snapshot &snapshot) {
  switch (snapshot.state) {
  case AnalysisState::CheckingSupport:
    return "Cancel chapter analysis (checking support)";
  case AnalysisState::WaitingForPlayback:
    return "Cancel chapter analysis (waiting for GPU)";
  case AnalysisState::Analyzing:
    if (snapshot.progress) {
      const int percent = static_cast<int>(
          std::lround(std::clamp(*snapshot.progress, 0.0, 1.0) * 100.0));
      return "Cancel chapter analysis (" + std::to_string(percent) + "%)";
    }
    return "Cancel chapter analysis";
  case AnalysisState::Installing:
    return "Cancel chapter model installation";
  case AnalysisState::Disabled:
  case AnalysisState::SetupRequired:
  case AnalysisState::Ready:
  case AnalysisState::Unsupported:
  case AnalysisState::Failed:
    break;
  }
  return "Cancel chapter analysis";
}

} // namespace

std::vector<ActionItem> buildActionCatalog(const ActionContext &context) {
  if (!context.requestActive) {
    return {{Action::StartAnalysis, "Analyze video chapters"}};
  }

  switch (context.snapshot.state) {
  case AnalysisState::Disabled:
    return {{Action::StartAnalysis, "Analyze video chapters"}};
  case AnalysisState::CheckingSupport:
  case AnalysisState::WaitingForPlayback:
  case AnalysisState::Analyzing:
    return {{Action::CancelAnalysis, runningLabel(context.snapshot)}};
  case AnalysisState::SetupRequired:
    return {{Action::InstallModels, "Install chapter models"},
            {Action::ShowStatus, "Chapter analysis details"}};
  case AnalysisState::Installing:
    return {{Action::CancelInstallation, runningLabel(context.snapshot)}};
  case AnalysisState::Ready:
    return {{Action::TogglePanel, context.panelOpen
                                         ? "Hide chapters"
                                         : "Show chapters"}};
  case AnalysisState::Unsupported:
    return {{Action::ShowStatus, "Chapter analysis unavailable"}};
  case AnalysisState::Failed:
    return {{Action::RetryAnalysis, "Retry chapter analysis"},
            {Action::ShowStatus, "Chapter analysis details"}};
  }
  return {};
}

} // namespace playback_video_chapters
