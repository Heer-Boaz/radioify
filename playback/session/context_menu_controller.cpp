#include "playback/session/context_menu_controller.h"

#include <algorithm>
#include <cmath>

namespace playback_session {
namespace {

constexpr playback_overlay::ContextMenuItemToken itemToken(
    playback_video_edit::Command command) {
  return static_cast<playback_overlay::ContextMenuItemToken>(command);
}

}  // namespace

std::optional<size_t> ContextMenuController::itemIndex(
    playback_video_edit::Command command) const {
  for (size_t index = 0; index < items_.size(); ++index) {
    if (items_[index].command == command) return index;
  }
  return std::nullopt;
}

std::optional<size_t> ContextMenuController::itemIndex(
    playback_overlay::ContextMenuItemToken token) const {
  for (size_t index = 0; index < items_.size(); ++index) {
    if (itemToken(items_[index].command) == token) return index;
  }
  return std::nullopt;
}

void ContextMenuController::refresh(
    const playback_video_edit::EditSnapshot& edit,
    const playback_video_edit::ExportProgress& editExport) {
  std::optional<playback_video_edit::Command> selectedCommand;
  if (selected_ < items_.size()) selectedCommand = items_[selected_].command;

  std::vector<Item> next;
  if (!edit.active) {
    next.push_back({playback_video_edit::Command::Open,
                    edit.hasEdits ? "Resume editing" : "Edit video"});
  }
  if (editExport.running()) {
    next.push_back({playback_video_edit::Command::Export, "Cancel save"});
  } else if (edit.hasUnexportedChanges) {
    next.push_back({playback_video_edit::Command::Export,
                    "Save edited copy"});
    next.push_back({playback_video_edit::Command::Discard,
                    "Discard changes"});
  }
  if (edit.active) {
    next.push_back({playback_video_edit::Command::Close, "Finish editing"});
  }
  items_ = std::move(next);
  if (items_.empty()) {
    selected_ = 0;
    visible_ = false;
    return;
  }
  if (selectedCommand) {
    if (const auto retained = itemIndex(*selectedCommand)) {
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
    playback_overlay::ContextMenuItemToken token) {
  if (!visible_) return false;
  const auto index = itemIndex(token);
  if (!index || *index == selected_) return false;
  selected_ = *index;
  return true;
}

std::optional<playback_video_edit::Command>
ContextMenuController::activateSelection() {
  if (!visible_ || selected_ >= items_.size()) return std::nullopt;
  const auto command = items_[selected_].command;
  visible_ = false;
  return command;
}

std::optional<playback_video_edit::Command> ContextMenuController::activate(
    playback_overlay::ContextMenuItemToken token) {
  const auto index = itemIndex(token);
  if (!visible_ || !index) return std::nullopt;
  const auto command = items_[*index].command;
  visible_ = false;
  return command;
}

playback_overlay::ContextMenuSnapshot ContextMenuController::snapshotFor(
    ContextMenuSurface surface) const {
  playback_overlay::ContextMenuSnapshot snapshot;
  if (!visible_ || surface != surface_ || items_.empty()) return snapshot;
  snapshot.visible = true;
  snapshot.anchorXRatio = anchorXRatio_;
  snapshot.anchorYRatio = anchorYRatio_;
  snapshot.selectedItem = itemToken(
      items_[std::min(selected_, items_.size() - 1)].command);
  snapshot.items.reserve(items_.size());
  for (const Item& item : items_) {
    snapshot.items.push_back({itemToken(item.command), item.label});
  }
  return snapshot;
}

}  // namespace playback_session
