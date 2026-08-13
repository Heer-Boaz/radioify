#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

#include "playback/overlay/context_menu.h"
#include "playback/video/edit/view.h"

namespace playback_session {

enum class ContextMenuSurface : uint8_t {
  Terminal,
  VideoWindow,
};

enum class ContextMenuInputKind : uint8_t {
  Open,
  Dismiss,
  MoveSelection,
  SelectControl,
  ActivateSelection,
  ActivateControl,
};

struct ContextMenuInput {
  ContextMenuInputKind kind = ContextMenuInputKind::Dismiss;
  ContextMenuSurface surface = ContextMenuSurface::Terminal;
  double x = 0.0;
  double y = 0.0;
  int selectionDelta = 0;
  std::optional<playback_overlay::OverlayControlId> control;
};

struct ContextMenuInputResult {
  bool handled = false;
  std::optional<playback_overlay::OverlayControlId> activatedControl;
};

// Owns only popup presentation state. Edit decisions and command execution
// stay with the edit controller and playback session respectively.
class ContextMenuController {
 public:
  bool visible() const { return visible_; }
  void refresh(const playback_video_edit::EditSnapshot& edit,
               const playback_video_edit::ExportProgress& editExport);
  bool open(ContextMenuSurface surface, double xRatio, double yRatio);
  bool dismiss();
  bool moveSelection(int delta);
  bool select(playback_overlay::OverlayControlId control);
  std::optional<playback_overlay::OverlayControlId> activateSelection();
  std::optional<playback_overlay::OverlayControlId> activate(
      playback_overlay::OverlayControlId control);
  playback_overlay::ContextMenuSnapshot snapshotFor(
      ContextMenuSurface surface) const;

 private:
  std::optional<size_t> itemIndex(
      playback_overlay::OverlayControlId control) const;

  bool visible_ = false;
  ContextMenuSurface surface_ = ContextMenuSurface::Terminal;
  double anchorXRatio_ = 0.5;
  double anchorYRatio_ = 0.5;
  size_t selected_ = 0;
  std::vector<playback_overlay::ContextMenuItem> items_;
};

}  // namespace playback_session
