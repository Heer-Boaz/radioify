#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "playback/video/edit/view.h"

namespace playback_video_edit {

enum class TimelineCellKind : uint8_t {
  Kept,
  KeptAlternate,
  Selected,
};

enum class SceneSuggestionCellKind : uint8_t {
  None,
  Dialogue,
  Cutscene,
  MenuOrLoading,
  Selected,
};

struct OverlayModel {
  std::vector<TimelineCellKind> cells;
  std::vector<SceneSuggestionCellKind> sceneSuggestionCells;
  std::vector<int> sceneSuggestionBoundaryCells;
  int playheadCell = 0;
  std::optional<int> inCell;
  std::optional<int> outCell;
  std::vector<int> cutCells;
  std::vector<int> smoothCutCells;
  std::string status;
};

// A retained program edit is still the active playback even when its editing
// tools are closed. Keep that state visible without duplicating edit-mode UI.
std::string retainedProgramBadge(const EditSnapshot& edit);

// Single presentation policy shared by the event loop, ASCII layout, and
// framebuffer layout. This is intentionally independent from renderer state.
bool needsOverlayPresentation(const EditSnapshot& edit,
                              const ExportProgress& editExport,
                              Prompt prompt);

// Converts immutable edit/export snapshots into renderer-independent cells.
// ASCII and framebuffer targets consume exactly this same projection.
OverlayModel buildOverlayModel(const EditSnapshot& edit,
                               const ExportProgress* editExport,
                               Prompt prompt, int width,
                               double timelineProgress);

}  // namespace playback_video_edit
