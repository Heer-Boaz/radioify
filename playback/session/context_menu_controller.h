#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "playback/media_action_catalog.h"
#include "playback/overlay/context_menu.h"
#include "playback/video/chapter/action_catalog.h"
#include "playback/video/edit/command.h"
#include "playback/video/edit/view.h"

namespace playback_session {

using ContextMenuCommand =
    std::variant<playback_media_actions::Action, playback_video_edit::Command,
                 playback_video_chapters::Action>;

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

// Owns only popup presentation state. Media, chapter, and edit decisions and
// command execution stay with their respective session/domain owners.
class ContextMenuController {
 public:
  bool visible() const { return visible_; }
  void refresh(const playback_video_edit::EditSnapshot& edit,
               const playback_video_edit::ExportProgress& editExport,
               playback_media_actions::Context sourceContext,
               const playback_video_chapters::ActionContext& chapterContext);
  bool open(ContextMenuSurface surface, double xRatio, double yRatio);
  bool dismiss();
  bool moveSelection(int delta);
  bool select(playback_overlay::ContextMenuItemToken token);
  std::optional<ContextMenuCommand> activateSelection();
  std::optional<ContextMenuCommand> activate(
      playback_overlay::ContextMenuItemToken token);
  playback_overlay::ContextMenuSnapshot snapshotFor(
      ContextMenuSurface surface) const;

 private:
  struct Item {
    ContextMenuCommand command = playback_media_actions::Action::Play;
    std::string label;
    playback_overlay::ContextMenuItemToken token = 0;
  };

  struct CommandToken {
    ContextMenuCommand command = playback_media_actions::Action::Play;
    playback_overlay::ContextMenuItemToken token = 0;
  };

  std::optional<size_t> itemIndex(const ContextMenuCommand& command) const;
  std::optional<size_t> itemIndex(
      playback_overlay::ContextMenuItemToken token) const;
  playback_overlay::ContextMenuItemToken tokenFor(
      const ContextMenuCommand& command);

  bool visible_ = false;
  ContextMenuSurface surface_ = ContextMenuSurface::Terminal;
  double anchorXRatio_ = 0.5;
  double anchorYRatio_ = 0.5;
  size_t selected_ = 0;
  std::vector<Item> items_;
  std::vector<CommandToken> commandTokens_;
};

}  // namespace playback_session
