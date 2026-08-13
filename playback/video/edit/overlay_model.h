#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "playback/video/edit/view.h"

namespace playback_video_edit {

enum class TimelineCellKind : uint8_t {
  Kept,
  Selected,
};

struct OverlayModel {
  std::vector<TimelineCellKind> cells;
  int playheadCell = 0;
  std::optional<int> inCell;
  std::optional<int> outCell;
  std::vector<int> cutCells;
  std::string status;
};

// Converts immutable edit/export snapshots into renderer-independent cells.
// ASCII and framebuffer targets consume exactly this same projection.
OverlayModel buildOverlayModel(const EditSnapshot& edit,
                               const ExportProgress* editExport,
                               ExitPrompt exitPrompt, int width,
                               double timelineProgress);

}  // namespace playback_video_edit
