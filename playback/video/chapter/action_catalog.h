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
  TogglePanel,
  ShowStatus,
};

struct ActionContext {
  const Snapshot &snapshot;
  bool requestActive = false;
  bool panelOpen = false;
};

struct ActionItem {
  Action action = Action::StartAnalysis;
  std::string label;
};

// Builds the active-video chapter portion of a context menu. It mirrors the
// stable player control so lifecycle details and commands remain discoverable
// without opening an empty chapter panel.
std::vector<ActionItem> buildActionCatalog(const ActionContext &context);

} // namespace playback_video_chapters
