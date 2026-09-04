#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "playback/video/chapter/chapter.h"

namespace playback_video_chapters {

// Metadata added to the existing timeline-preview popover once generated
// chapters exist. Non-ready states return no lines and therefore retain the
// established frame-only preview geometry.
std::vector<std::string> previewMetadata(const Snapshot &snapshot,
                                         std::int64_t targetUs);

struct OverviewPanelLayout {
  enum class TextRole : std::uint8_t {
    Body,
    Accent,
  };

  struct TextRun {
    int column = 0;
    std::string text;
    TextRole role = TextRole::Body;
  };

  struct Line {
    std::vector<TextRun> runs;
    std::optional<std::int64_t> chapterStartUs;
  };

  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool drawer = false;
  int scrollOffset = 0;
  int maximumScrollOffset = 0;
  int pageLineCount = 0;
  int closeButtonColumn = -1;
  int closeButtonWidth = 0;
  // Wrapping preserves semantic runs. Renderers never have to reconstruct
  // title/heading styling from a flattened string or from a row index.
  std::vector<Line> lines;

  bool drawable() const { return width >= 12 && height >= 4; }
};

// Both renderers consume the same responsive cell layout. Only a ready
// analysis is drawable: a wide surface gets a right-hand drawer and a
// genuinely narrow surface gets a full-width overlay.
OverviewPanelLayout layoutOverviewPanel(const Snapshot &snapshot, int columns,
                                        int rows, int chromeTopY,
                                        int requestedScrollOffset = 0);

} // namespace playback_video_chapters
