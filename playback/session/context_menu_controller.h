#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "playback/overlay/context_menu.h"
#include "playback/video/edit/command.h"
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
  SelectItem,
  ActivateSelection,
  ActivateItem,
};

struct ContextMenuInput {
  ContextMenuInputKind kind = ContextMenuInputKind::Dismiss;
  ContextMenuSurface surface = ContextMenuSurface::Terminal;
  double x = 0.0;
  double y = 0.0;
  int selectionDelta = 0;
  std::optional<playback_overlay::ContextMenuItemToken> item;
  // Present only when the menu was opened on the program progress bar.
  // This lets the editor select an edit point without moving the playhead.
  std::optional<int64_t> timelineUs;
  int64_t timelineToleranceUs = 0;
};

// Owns only popup presentation state. Edit decisions and command execution
// stay with the video-edit workspace.
class ContextMenuController {
 public:
  bool visible() const { return visible_; }
  void refresh(const playback_video_edit::EditSnapshot& edit,
               const playback_video_edit::ExportProgress& editExport);
  bool open(ContextMenuSurface surface, double xRatio, double yRatio);
  bool dismiss();
  bool moveSelection(int delta);
  bool select(playback_overlay::ContextMenuItemToken token);
  std::optional<playback_video_edit::Command> activateSelection();
  std::optional<playback_video_edit::Command> activate(
      playback_overlay::ContextMenuItemToken token);
  playback_overlay::ContextMenuSnapshot snapshotFor(
      ContextMenuSurface surface) const;

 private:
  struct Item {
    playback_video_edit::Command command = playback_video_edit::Command::Open;
    std::string label;
  };

  std::optional<size_t> itemIndex(playback_video_edit::Command command) const;
  std::optional<size_t> itemIndex(
      playback_overlay::ContextMenuItemToken token) const;

  bool visible_ = false;
  ContextMenuSurface surface_ = ContextMenuSurface::Terminal;
  double anchorXRatio_ = 0.5;
  double anchorYRatio_ = 0.5;
  size_t selected_ = 0;
  std::vector<Item> items_;
};

}  // namespace playback_session
