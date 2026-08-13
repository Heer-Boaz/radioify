#include "playback/session/context_menu_controller.h"

#include <algorithm>
#include <cmath>

namespace playback_session {

std::optional<size_t> ContextMenuController::itemIndex(
    playback_overlay::OverlayControlId control) const {
  for (size_t index = 0; index < items_.size(); ++index) {
    if (items_[index].control == control) return index;
  }
  return std::nullopt;
}

void ContextMenuController::refresh(
    const playback_video_edit::EditSnapshot& edit,
    const playback_video_edit::ExportProgress& editExport) {
  std::optional<playback_overlay::OverlayControlId> selectedControl;
  if (selected_ < items_.size()) selectedControl = items_[selected_].control;

  std::vector<playback_overlay::ContextMenuItem> next;
  if (!edit.active) {
    next.push_back({playback_overlay::OverlayControlId::EditOpen,
                    edit.hasEdits ? "Resume editing" : "Edit video"});
  }
  if (editExport.running()) {
    next.push_back(
        {playback_overlay::OverlayControlId::EditExport, "Cancel save"});
  } else if (edit.dirty) {
    next.push_back({playback_overlay::OverlayControlId::EditExport,
                    "Save edited copy"});
    next.push_back({playback_overlay::OverlayControlId::EditDiscard,
                    "Discard changes"});
  }
  if (edit.active) {
    next.push_back(
        {playback_overlay::OverlayControlId::EditDone, "Finish editing"});
  }
  items_ = std::move(next);
  if (items_.empty()) {
    selected_ = 0;
    visible_ = false;
    return;
  }
  if (selectedControl) {
    if (const auto retained = itemIndex(*selectedControl)) {
      selected_ = *retained;
      return;
    }
  }
  selected_ = std::min(selected_, items_.size() - 1);
}

bool ContextMenuController::open(ContextMenuSurface surface, double xRatio,
                                 double yRatio) {
  if (items_.empty()) return false;
  surface_ = surface;
  anchorXRatio_ = std::clamp(std::isfinite(xRatio) ? xRatio : 0.5, 0.0, 1.0);
  anchorYRatio_ = std::clamp(std::isfinite(yRatio) ? yRatio : 0.5, 0.0, 1.0);
  selected_ = std::min(selected_, items_.size() - 1);
  visible_ = true;
  return true;
}

bool ContextMenuController::dismiss() {
  if (!visible_) return false;
  visible_ = false;
  return true;
}

bool ContextMenuController::moveSelection(int delta) {
  if (!visible_ || items_.empty() || delta == 0) return false;
  const int count = static_cast<int>(items_.size());
  const int current = static_cast<int>(selected_);
  selected_ = static_cast<size_t>((current + (delta % count) + count) % count);
  return true;
}

bool ContextMenuController::select(
    playback_overlay::OverlayControlId control) {
  if (!visible_) return false;
  const auto index = itemIndex(control);
  if (!index || *index == selected_) return false;
  selected_ = *index;
  return true;
}

std::optional<playback_overlay::OverlayControlId>
ContextMenuController::activateSelection() {
  if (!visible_ || selected_ >= items_.size()) return std::nullopt;
  const auto control = items_[selected_].control;
  visible_ = false;
  return control;
}

std::optional<playback_overlay::OverlayControlId>
ContextMenuController::activate(playback_overlay::OverlayControlId control) {
  if (!visible_ || !itemIndex(control)) return std::nullopt;
  visible_ = false;
  return control;
}

playback_overlay::ContextMenuSnapshot ContextMenuController::snapshotFor(
    ContextMenuSurface surface) const {
  playback_overlay::ContextMenuSnapshot snapshot;
  if (!visible_ || surface != surface_ || items_.empty()) return snapshot;
  snapshot.visible = true;
  snapshot.anchorXRatio = anchorXRatio_;
  snapshot.anchorYRatio = anchorYRatio_;
  snapshot.selectedControlToken = playback_overlay::overlayControlToken(
      items_[std::min(selected_, items_.size() - 1)].control);
  snapshot.items = items_;
  return snapshot;
}

}  // namespace playback_session
