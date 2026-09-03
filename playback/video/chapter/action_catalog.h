#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "playback/video/chapter/chapter.h"

namespace playback_video_chapters {

// Semantic chapter commands exposed by playback surfaces. Presentation owns
// labels and tokens; the active playback session remains the sole executor.
enum class Action : std::uint8_t {
  StartAnalysis,
  CancelAnalysis,
  InstallModels,
  CancelInstallation,
  RetryAnalysis,
  ToggleOverview,
  ShowStatus,
};

struct ActionContext {
  const Snapshot &snapshot;
  bool requestActive = false;
  bool overviewOpen = false;
};

struct ActionItem {
  Action action = Action::StartAnalysis;
  std::string label;
};

// Builds the active-video chapter portion of a context menu. Running and
// terminal states remain discoverable here without reserving toolbar space or
// opening an empty overview panel.
std::vector<ActionItem> buildActionCatalog(const ActionContext &context);

} // namespace playback_video_chapters
