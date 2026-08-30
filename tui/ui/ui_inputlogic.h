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

struct BrowserInteractionState {
  browser_input::EntryClickTracker entryClicks;
  BrowserPointerState pointer;
  int breadcrumbHover = -1;
  int actionHover = -1;
  bool searchBarHover = false;
};

struct BrowserInputLayout {
  const GridLayout& entries;
  const BreadcrumbLine& breadcrumbs;
  int breadcrumbY = -1;
  int searchBarY = -1;
  int searchBarWidth = 0;
  int listTop = 0;
  int listHeight = 0;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  const ActionStripLayout& actionStrip;
};

struct BrowserInputCapabilities {
  bool interactionEnabled = true;
  bool playMode = true;
  bool decoderReady = false;
};

struct BrowserInputResult {
  bool dirty = false;
  bool quitRequested = false;
  std::vector<tui_input::Command> commands;
};

bool setBrowserHoveredEntry(BrowserState& browser, int entryIndex);

BrowserInputResult handleInputEvent(
    const InputEvent& event, BrowserNavigator& navigator,
    BrowserInteractionState& interaction, const BrowserInputLayout& layout,
    const BrowserInputCapabilities& capabilities);
