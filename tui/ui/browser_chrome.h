#pragma once

#include <optional>
#include <string>
#include <vector>

#include "audio/playback_snapshot.h"
#include "playback/control/system_control_state.h"
#include "playback/session/presentation_policy.h"
#include "tui/ui/browser_action_strip.h"
#include "tui/ui/ui_footer_layout.h"

namespace browser_chrome {

// Named inputs for the browser's playback chrome. This is the UX policy
// boundary between runtime state and visible controls/layout.
struct Input {
  AudioPlaybackSnapshot audio;
  std::optional<PlaybackControlState> playback;
  std::optional<PlaybackPresentationState> videoPresentation;
  bool playbackTargetAvailable = false;
  bool audioPictureInPictureOpen = false;
  bool melodyVisualizationActive = false;
  bool transportUiEnabled = false;
  bool optionsModeActive = false;
  bool selectedEntryHasOptions = false;
  bool hasWarning = false;
  bool hasMediaTaskStatus = false;
  BrowserState::ViewMode viewMode = BrowserState::ViewMode::ListOnly;
  std::string nowPlayingLabel;
  int width = 0;
};

struct Model {
  BrowserFooterLayout footer;
  std::vector<browser_action_strip::Item> actions;
  std::string nowPlayingLabel;
};

Model build(const Input& input);

}  // namespace browser_chrome
