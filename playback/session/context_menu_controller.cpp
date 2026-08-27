#include "playback/session/context_menu_controller.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace playback_session {

std::optional<size_t> ContextMenuController::itemIndex(
    const ContextMenuCommand& command) const {
  for (size_t index = 0; index < items_.size(); ++index) {
    if (items_[index].command == command) return index;
  }
  return std::nullopt;
}

std::optional<size_t> ContextMenuController::itemIndex(
    playback_overlay::ContextMenuItemToken token) const {
  for (size_t index = 0; index < items_.size(); ++index) {
    if (items_[index].token == token) return index;
  }
  return std::nullopt;
}

playback_overlay::ContextMenuItemToken ContextMenuController::tokenFor(
    const ContextMenuCommand& command) {
  const auto retained = std::find_if(
      commandTokens_.begin(), commandTokens_.end(),
      [&](const CommandToken& registered) {
        return registered.command == command;
      });
  if (retained != commandTokens_.end()) return retained->token;

  // Tokens are opaque presentation identities. The registry is bounded by the
  // closed media-action and edit-command sets, so enum values and domains never
  // need to be encoded into the token itself.
  const auto token = static_cast<playback_overlay::ContextMenuItemToken>(
      commandTokens_.size() + 1u);
  commandTokens_.push_back({command, token});
  return token;
}

void ContextMenuController::refresh(
    const playback_video_edit::EditSnapshot& edit,
    const playback_video_edit::ExportProgress& editExport,
    playback_media_actions::Context sourceContext) {
  std::optional<ContextMenuCommand> selectedCommand;
  if (selected_ < items_.size()) selectedCommand = items_[selected_].command;

  std::vector<Item> next;
  sourceContext.mediaKind = playback_media_actions::MediaKind::Video;
  sourceContext.currentPlayback = true;
  sourceContext.editorActive = edit.active;
  sourceContext.hasEdits = edit.hasEdits;
  for (playback_media_actions::Item& item :
       playback_media_actions::build(sourceContext)) {
    next.push_back({item.action, std::move(item.label)});
  }

  if (edit.active) {
    if (edit.sceneAnalysisStatus ==
        playback_video_edit::SceneAnalysisStatus::Running) {
      next.push_back({playback_video_edit::Command::CancelSceneAnalysis,
                      "Cancel segment detection"});
    } else if (edit.sceneAnalysisStatus ==
               playback_video_edit::SceneAnalysisStatus::Ready) {
      next.push_back({playback_video_edit::Command::ToggleSceneSuggestions,
                      edit.suggestionReview.visible
                          ? "Hide suggestions"
                          : "Show suggestions"});
      next.push_back({playback_video_edit::Command::StartSceneAnalysis,
                      "Detect segments again"});
    } else {
      next.push_back(
          {playback_video_edit::Command::StartSceneAnalysis,
           edit.sceneAnalysisStatus ==
                   playback_video_edit::SceneAnalysisStatus::Failed
               ? "Retry segment detection"
               : "Detect segments..."});
    }
    if (edit.suggestionReview.visible) {
      if (edit.suggestionReview.selectedId) {
        next.push_back({playback_video_edit::Command::SelectSceneSuggestion,
                        "Select suggested segment"});
        next.push_back({playback_video_edit::Command::DismissSceneSuggestion,
                        "Hide suggestion"});
      }
      if (edit.suggestionReview.canUndoHide) {
        next.push_back(
            {playback_video_edit::Command::UndoDismissSceneSuggestion,
             "Undo hidden suggestion"});
      }
    }
    if (edit.inTimelineUs) {
      next.push_back(
          {playback_video_edit::Command::ClearIn, "Clear selection start"});
    }
    if (edit.outTimelineUs) {
      next.push_back(
          {playback_video_edit::Command::ClearOut, "Clear selection end"});
    }
    if (edit.inTimelineUs && edit.outTimelineUs) {
      next.push_back({playback_video_edit::Command::ClearInAndOut,
                      "Cancel selection"});
      if (edit.canRippleDelete) {
        next.push_back({playback_video_edit::Command::RippleDelete,
                        "Remove selected section"});
      }
      if (edit.canTrim) {
        next.push_back({playback_video_edit::Command::Trim,
                        "Keep only selected section"});
      }
    }
    if (edit.canToggleSmoothCut && edit.selectedCutTransition) {
      const bool smooth = edit.selectedCutTransition->kind ==
                          playback_video_edit::CutTransitionKind::MotionSmooth;
      next.push_back({playback_video_edit::Command::ToggleSmoothCut,
                      smooth ? "Hard cut" : "Smooth cut"});
    }
    if (edit.canUndo) {
      next.push_back({playback_video_edit::Command::Undo, "Undo"});
    }
    if (edit.canRedo) {
      next.push_back({playback_video_edit::Command::Redo, "Redo"});
    }
    if (edit.hasEdits) {
      next.push_back(
          {playback_video_edit::Command::Reset, "Reset all edits"});
    }
  }
  if (editExport.running()) {
    next.push_back(
        {playback_video_edit::Command::CancelExport,
         editExport.targetsCurrentRevision ? "Cancel export"
                                           : "Cancel older export"});
  } else if (edit.hasEdits) {
    // Exporting establishes a clean revision; it does not consume the edit
    // document. The same revision may be exported again or discarded.
    next.push_back(
        {playback_video_edit::Command::StartExport,
         editExport.failed() ? "Retry export" : "Export edited copy"});
    next.push_back({playback_video_edit::Command::RequestDiscard,
                    "Discard changes"});
  }
  if (edit.active) {
    next.push_back({playback_video_edit::Command::Finish, "Done editing"});
  }
  for (Item& item : next) item.token = tokenFor(item.command);
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

std::optional<ContextMenuCommand> ContextMenuController::activateSelection() {
  if (!visible_ || selected_ >= items_.size()) return std::nullopt;
  const auto command = items_[selected_].command;
  visible_ = false;
  return command;
}

std::optional<ContextMenuCommand> ContextMenuController::activate(
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
  snapshot.selectedItem =
      items_[std::min(selected_, items_.size() - 1)].token;
  snapshot.items.reserve(items_.size());
  for (const Item& item : items_) {
    snapshot.items.push_back({item.token, item.label});
  }
  return snapshot;
}

}  // namespace playback_session
