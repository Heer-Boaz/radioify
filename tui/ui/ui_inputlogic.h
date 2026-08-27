#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <variant>
#include <vector>

#include "browser_action_strip.h"
#include "browser_model.h"
#include "input_event.h"
#include "playback/input/command.h"
#include "playback/input/shortcuts.h"

namespace browser_input {

inline constexpr bool pointerActivatesEntry(MouseEventKind kind) {
  return kind == MouseEventKind::DoubleClick;
}

// The platform identifies a double-click by time and pointer position. The
// browser additionally owns which entry received the first press so a reload
// or layout change cannot activate a different item on the second press.
class EntryClickTracker {
 public:
  void recordPress(const BrowserEntry& entry);
  std::optional<BrowserState::EntryIdentity> consumeDoubleClickAnchor();
  void reset();

 private:
  std::optional<BrowserState::EntryIdentity> anchor_;
};

}  // namespace browser_input

namespace tui_input {

struct PlaybackCommand {
  playback_input::Command command;
};
struct Resize {};
struct ActivateEntry {
  BrowserEntry entry;
};
struct OpenFileContextMenu {
  BrowserEntry entry;
  int x = -1;
  int y = -1;
};
struct RenderFile {
  std::filesystem::path file;
};

using Command = std::variant<PlaybackCommand, Resize, ActivateEntry,
                             OpenFileContextMenu, RenderFile>;

}  // namespace tui_input

class BrowserNavigator;

enum class PlaybackInputResult : uint8_t {
  Ignored,
  Handled,
  HandledWithoutOverlayRefresh,
};

struct PlaybackInputMatch {
  playback_input::Command command;
  PlaybackInputResult result = PlaybackInputResult::Ignored;
};

inline std::optional<PlaybackInputMatch> matchPlaybackInput(
    const InputEvent& ev,
    uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                kPlaybackShortcutContextShared) {
  const std::optional<PlaybackAction> action =
      resolvePlaybackAction(ev, shortcutContexts);
  if (!action) return std::nullopt;

  PlaybackInputResult result = PlaybackInputResult::Handled;
  switch (*action) {
    case PlaybackAction::CopyVideoFrame:
    case PlaybackAction::ExitPlaybackSession:
    case PlaybackAction::DismissPictureInPicture:
    case PlaybackAction::CloseViewer:
      result = PlaybackInputResult::HandledWithoutOverlayRefresh;
      break;
    default:
      break;
  }
  return PlaybackInputMatch{playback_input::Command{*action}, result};
}

enum class BrowserSearchFocus {
  None,
  Filter,
  PathSearch,
};

void setBrowserSearchFocus(BrowserState& browser, BrowserSearchFocus focus,
                           bool& dirty);

class BrowserPointerState {
 public:
  void pressAction(ActionStripItem action) { pressedAction_ = action; }
  std::optional<ActionStripItem> releaseAction(
      std::optional<ActionStripItem> releasedOver);
  bool hasPressedAction() const { return pressedAction_.has_value(); }
  void cancelPress() { pressedAction_.reset(); }

 private:
  std::optional<ActionStripItem> pressedAction_;
};

bool setBrowserHoveredEntry(BrowserState& browser, int entryIndex);

void handleInputEvent(const InputEvent& ev, BrowserNavigator& navigator,
                      browser_input::EntryClickTracker& entryClickTracker,
                      BrowserPointerState& pointerState,
                      const GridLayout& layout,
                      const BreadcrumbLine& breadcrumbLine, int breadcrumbY,
                      int searchBarY, int searchBarWidth, int listTop,
                      int listHeight, int progressBarX, int progressBarY,
                      int progressBarWidth,
                      const ActionStripLayout& actionStrip,
                      bool browserInteractionEnabled, bool playMode,
                      bool decoderReady, int& breadcrumbHover,
                      int& actionHover, bool& searchBarHover, bool& dirty,
                      bool& running,
                      std::vector<tui_input::Command>& commands);
