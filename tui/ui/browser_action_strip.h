#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "browser_model.h"
#include "playback/overlay/overlay.h"

enum class ActionStripItem {
  Previous,
  PlayPause,
  Next,
  Radio,
  Hz50,
  PitchMonitor,
  View,
  PictureInPicture,
  Options
};

struct ActionStripButton {
  ActionStripItem id = ActionStripItem::Radio;
  int x0 = 0;
  int x1 = 0;
  int y = 0;
};

struct ActionStripLayout {
  int y = -1;
  std::vector<ActionStripButton> buttons;
};

namespace browser_action_strip {

struct Input {
  playback_overlay::PlaybackOverlayState playback;
  bool pitchMonitorAvailable = false;
  bool pitchMonitorActive = false;
  bool browserControlsAvailable = false;
  BrowserState::ViewMode viewMode = BrowserState::ViewMode::ListOnly;
  bool optionsAvailable = false;
  bool optionsActive = false;
};

struct Item {
  ActionStripItem id = ActionStripItem::Radio;
  std::string label;
  std::string hoverLabel;
  bool active = false;
  int width = 0;
};

struct Placement {
  std::size_t itemIndex = 0;
  int x = 0;
  int y = 0;
  int width = 0;
};

struct Layout {
  std::vector<Placement> placements;
  int lineCount = 0;
};

std::vector<Item> build(const Input& input);
Layout layout(const std::vector<Item>& items, int width, int top = 0);
int wrappedLineCount(const std::vector<Item>& items, int width);

}  // namespace browser_action_strip
