#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "playback/video/edit/export.h"
#include "playback/video/edit/timeline.h"

namespace playback_video_edit {

enum class TimelineCellKind : uint8_t {
  Kept,
  Removed,
  Selected,
};

struct OverlayModel {
  std::vector<TimelineCellKind> cells;
  int playheadCell = 0;
  std::optional<int> inCell;
  std::optional<int> outCell;
  std::string status;
};

// Converts immutable edit/export snapshots into renderer-independent cells.
// ASCII and framebuffer targets consume exactly this same projection.
OverlayModel buildOverlayModel(const EditSnapshot& edit,
                               const ExportSnapshot* editExport, int width,
                               double sourceProgress);

}  // namespace playback_video_edit
