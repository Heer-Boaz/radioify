#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "playback/video/chapter/chapter.h"

namespace playback_video_chapters {

// Metadata added to the existing timeline-preview popover once generated
// chapters exist. Non-ready states return no lines and therefore retain the
// established frame-only preview geometry.
std::vector<std::string> previewMetadata(const Snapshot& snapshot,
                                         std::int64_t targetUs);

struct OverviewPanelLayout {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool drawer = false;
  std::vector<std::string> lines;

  bool drawable() const { return width >= 12 && height >= 4; }
};

// Both renderers consume the same responsive cell layout. Only a ready
// analysis is drawable: a wide surface gets a right-hand drawer and a
// genuinely narrow surface gets a full-width overlay.
OverviewPanelLayout layoutOverviewPanel(const Snapshot& snapshot,
                                        int columns, int rows,
                                        int progressBarY);

std::string stateStatusLine(const Snapshot& snapshot);

}  // namespace playback_video_chapters
