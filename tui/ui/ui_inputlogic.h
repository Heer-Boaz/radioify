#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

#include "browser_model.h"
#include "input_event.h"
#include "playback/input/shortcut_types.h"

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

struct InputCallbacks {
  std::function<void()> onQuit;
  std::function<void()> onResize;
  std::function<void()> onStopPlayback;
  std::function<std::filesystem::path()> onCurrentPlaybackFile;
  std::function<void()> onPlay;
  std::function<void()> onPause;
  std::function<void()> onTogglePause;
  std::function<void()> onPlayPrevious;
  std::function<void()> onPlayNext;
  std::function<void()> onToggleWindow;
  std::function<void()> onTogglePictureInPicture;
  std::function<void()> onToggleFullscreen;
  std::function<void()> onToggleRadio;
  std::function<void()> onToggle50Hz;
  std::function<void()> onToggleSubtitles;
  std::function<void()> onToggleAudioTrack;
  std::function<void()> onToggleOptions;
  std::function<void(PlaybackShortcutAction)> onPlaybackContextShortcut;
  std::function<void(int)> onSeekBy;
  std::function<void()> onPreviousFrame;
  std::function<void()> onNextFrame;
  std::function<void()> onCopyVideoFrame;
  std::function<void(double)> onSeekToRatio;
  std::function<void(float)> onAdjustVolume;
  std::function<bool(const BrowserEntry&)> onActivateEntry;
  std::function<bool(const std::vector<std::filesystem::path>&)> onPlayFiles;
  std::function<void(const BrowserEntry&, int, int)> onOpenFileContextMenu;
  std::function<void(const std::filesystem::path&)> onRenderFile;
};

class BrowserNavigator;

enum class PlaybackInputResult : uint8_t {
  Ignored,
  Handled,
  HandledWithoutOverlayRefresh,
};

enum class BrowserSearchFocus {
  None,
  Filter,
  PathSearch,
};

void setBrowserSearchFocus(BrowserState& browser, BrowserSearchFocus focus,
                           bool& dirty);

enum class ActionStripItem {
  Previous,
  PlayPause,
  Next,
  Radio,
  Hz50,
  View,
  PictureInPicture,
  Options
};

struct ActionStripButton {
  ActionStripItem id = ActionStripItem::Radio;
  int x0 = 0;
  int x1 = 0;
  int y = 0;
};

struct ActionStripLayout {
  int y = -1;
  std::vector<ActionStripButton> buttons;
};

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

PlaybackInputResult handlePlaybackInput(
    const InputEvent& ev, const InputCallbacks& callbacks,
    uint32_t shortcutContexts = kPlaybackShortcutContextGlobal |
                                kPlaybackShortcutContextShared);

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
                      bool& running, const InputCallbacks& callbacks);
