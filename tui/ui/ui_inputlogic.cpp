#include "ui_inputlogic.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <utility>

#include "browser_navigation.h"
#include "browser_grid_index.h"
#include "browser_keymap.h"
#include "browser_search.h"
#include "consolescreen.h"
#include "optionsbrowser.h"
#include "playback/input/shortcuts.h"
#include "playback/overlay/interaction.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"
#include "ui_helpers.h"

namespace {

void publishPlaybackCommand(std::vector<tui_input::Command>& commands,
                            playback_input::Command command) {
  commands.emplace_back(tui_input::PlaybackCommand{std::move(command)});
}

bool isSelectableEntry(const BrowserEntry& entry) {
  return entry.isSelectable();
}

bool isMouseInSearchBar(const MouseEvent& mouse, int searchBarY,
                       int searchBarWidth) {
  return searchBarY >= 0 && searchBarWidth > 0 && mouse.pos.Y == searchBarY &&
         mouse.pos.X >= 0 && mouse.pos.X < searchBarWidth;
}

int nearestSelectableEntry(const std::vector<BrowserEntry>& entries, int start,
                           int direction) {
  if (entries.empty()) return 0;
  int n = static_cast<int>(entries.size());
  int idx = std::clamp(start, 0, n - 1);
  int step = direction >= 0 ? 1 : -1;
  for (int i = 0; i < n; ++i) {
    if (isSelectableEntry(entries[static_cast<size_t>(idx)])) return idx;
    idx += step;
    if (idx >= n) idx = 0;
    if (idx < 0) idx = n - 1;
  }
  return start;
}

void getRowColFromIndex(int idx, const GridLayout& layout,
                        BrowserState::ViewMode mode, int& row, int& col) {
  if (mode == BrowserState::ViewMode::ListOnly) {
    int stride = std::max(1, layout.rowsVisible);
    if (stride <= 0) {
      row = 0;
      col = 0;
    } else {
      col = idx / stride;
      row = idx % stride;
    }
  } else {
    if (layout.cols <= 0) {
      row = 0;
      col = 0;
    } else {
      row = idx / layout.cols;
      col = idx % layout.cols;
    }
  }
}

void moveSelection(BrowserState& browser, const GridLayout& layout,
                   int deltaCol, int deltaRow, bool& dirty) {
  int count = static_cast<int>(browser.entries.size());
  if (count == 0 || layout.totalRows <= 0 || layout.cols <= 0) return;

  if (browser.viewMode == BrowserState::ViewMode::ListOnly) {
    int idx = browser.selected;
    if (deltaCol != 0) {
      idx += deltaCol * std::max(1, layout.rowsVisible);
    } else if (deltaRow != 0) {
      idx += deltaRow;
    }
    idx = std::clamp(idx, 0, count - 1);
    int direction = (deltaCol > 0 || deltaRow > 0) ? 1 : -1;
    idx = nearestSelectableEntry(browser.entries, idx, direction);
    if (idx != browser.selected) {
      browser.selected = idx;
      ensureBrowserSelectionVisible(browser, layout);
      dirty = true;
    }
    return;
  }

  int row = 0;
  int col = 0;
  getRowColFromIndex(browser.selected, layout, browser.viewMode, row, col);

  int nextRow = std::clamp(row + deltaRow, 0, layout.totalRows - 1);
  int nextCol = std::clamp(col + deltaCol, 0, layout.cols - 1);

  int idx = browserGridEntryIndex(layout, browser.viewMode, nextRow, nextCol,
                                  count);
  if (idx < 0) {
    if (deltaCol > 0 || deltaRow > 0)
      idx = count - 1;
    else
      idx = 0;
  }

  int direction = (deltaCol > 0 || deltaRow > 0) ? 1 : -1;
  idx = nearestSelectableEntry(browser.entries, idx, direction);

  if (idx != browser.selected) {
    browser.selected = idx;
    ensureBrowserSelectionVisible(browser, layout);
    dirty = true;
  }
}

void pageSelection(BrowserState& browser, const GridLayout& layout,
                   int direction, bool& dirty) {
  int count = static_cast<int>(browser.entries.size());
  if (count == 0 || layout.totalRows <= 0 || layout.cols <= 0) return;

  if (browser.viewMode == BrowserState::ViewMode::ListOnly) {
    int step = std::max(1, layout.rowsVisible);
    int idx =
        std::clamp(browser.selected + direction * step, 0, count - 1);
    idx = nearestSelectableEntry(browser.entries, idx, direction);
    if (idx != browser.selected) {
      browser.selected = idx;
      ensureBrowserSelectionVisible(browser, layout);
      dirty = true;
    }
    return;
  }

  int row = 0;
  int col = 0;
  getRowColFromIndex(browser.selected, layout, browser.viewMode, row, col);

  int step = std::max(1, layout.rowsVisible);
  int nextRow = std::clamp(row + direction * step, 0, layout.totalRows - 1);

  int idx =
      browserGridEntryIndex(layout, browser.viewMode, nextRow, col, count);
  if (idx < 0) idx = count - 1;
  idx = nearestSelectableEntry(browser.entries, idx, direction);

  if (idx != browser.selected) {
    browser.selected = idx;
    ensureBrowserSelectionVisible(browser, layout);
    dirty = true;
  }
}

BrowserState::ViewMode nextViewMode(BrowserState::ViewMode mode) {
  switch (mode) {
    case BrowserState::ViewMode::Thumbnails:
      return BrowserState::ViewMode::ListPreview;
    case BrowserState::ViewMode::ListPreview:
      return BrowserState::ViewMode::ListOnly;
    case BrowserState::ViewMode::ListOnly:
      return BrowserState::ViewMode::Thumbnails;
  }
  return BrowserState::ViewMode::Thumbnails;
}

BrowserState::ViewMode prevViewMode(BrowserState::ViewMode mode) {
  switch (mode) {
    case BrowserState::ViewMode::Thumbnails:
      return BrowserState::ViewMode::ListOnly;
    case BrowserState::ViewMode::ListPreview:
      return BrowserState::ViewMode::Thumbnails;
    case BrowserState::ViewMode::ListOnly:
      return BrowserState::ViewMode::ListPreview;
  }
  return BrowserState::ViewMode::Thumbnails;
}

void scrollFromBar(BrowserState& browser, const GridLayout& layout, int y,
                   int listTop, int listHeight, bool& dirty) {
  if (!layout.showScrollBar || layout.totalRows <= layout.rowsVisible) return;
  int maxScroll = layout.totalRows - layout.rowsVisible;
  if (maxScroll <= 0) return;
  int barHeight = std::max(1, listHeight);
  int thumbHeight = std::max(
      1, static_cast<int>((static_cast<int64_t>(layout.rowsVisible) *
                               barHeight +
                           layout.totalRows - 1) /
                          layout.totalRows));
  if (thumbHeight > barHeight) thumbHeight = barHeight;
  int thumbTravel = barHeight - thumbHeight;
  int rel = y - listTop;
  rel = std::clamp(rel, 0, barHeight - 1);
  int target = 0;
  if (thumbTravel > 0) {
    int thumbPos = std::clamp(rel - thumbHeight / 2, 0, thumbTravel);
    target = static_cast<int>(
        (static_cast<int64_t>(thumbPos) * maxScroll + thumbTravel / 2) /
        thumbTravel);
  }
  target = std::clamp(target, 0, maxScroll);
  if (target != browser.scrollRow) {
    browser.scrollRow = target;
    dirty = true;
  }
}

int actionStripIndexAt(const ActionStripLayout& layout, int x, int y) {
  if (layout.y < 0) return -1;
  int count = static_cast<int>(layout.buttons.size());
  for (int i = 0; i < count; ++i) {
    const auto& btn = layout.buttons[static_cast<size_t>(i)];
    if (y == btn.y && x >= btn.x0 && x < btn.x1) return i;
  }
  return -1;
}

int browserEntryIndexAt(const BrowserState& browser, const GridLayout& layout,
                        int x, int y, int listTop) {
  const int count = static_cast<int>(browser.entries.size());
  if (count == 0 || layout.cols <= 0) {
    return -1;
  }
  const int cellHeight = std::max(1, layout.cellHeight);
  const int visibleHeight = layout.rowsVisible * cellHeight;
  if (y < listTop || y >= listTop + visibleHeight) {
    return -1;
  }
  const int row = (y - listTop) / cellHeight + browser.scrollRow;
  const int col = layout.colWidth > 0 ? x / layout.colWidth : 0;
  if (col < 0 || col >= layout.cols) {
    return -1;
  }
  const int index = browserGridEntryIndex(layout, browser.viewMode, row, col,
                                          count);
  if (index < 0 || index >= count ||
      !browser.entries[static_cast<std::size_t>(index)].isSelectable()) {
    return -1;
  }
  return index;
}
}  // namespace

namespace {

class BrowserInputController {
 public:
  BrowserInputController(BrowserNavigator& navigator,
                         BrowserInteractionState& interaction,
                         const BrowserInputLayout& inputLayout,
                         const BrowserInputCapabilities& capabilities)
      : navigator(navigator),
        browser(navigator.state()),
        interaction(interaction),
        inputLayout(inputLayout),
        capabilities(capabilities) {}

  BrowserInputResult handle(const InputEvent& event) {
    handleEvent(event);
    return std::move(result);
  }

 private:
  void handleEvent(const InputEvent& ev) {
    const std::optional<BrowserState::EntryIdentity> doubleClickAnchor =
        updateEntryClickTracking(ev);
    switch (ev.type) {
      case InputEvent::Type::PointerLeave:
        handlePointerLeave();
        break;
      case InputEvent::Type::Resize:
        result.dirty = true;
        result.commands.emplace_back(tui_input::Resize{});
        break;
      case InputEvent::Type::Action:
        handleAction(ev);
        break;
      case InputEvent::Type::Key:
        handleKey(ev);
        break;
      case InputEvent::Type::Mouse:
        handleMouse(ev.mouse, doubleClickAnchor);
        break;
      case InputEvent::Type::None:
      case InputEvent::Type::FileDrop:
        break;
    }
  }

  std::optional<BrowserState::EntryIdentity> updateEntryClickTracking(
      const InputEvent& event) {
    if (event.type != InputEvent::Type::Mouse) {
      interaction.entryClicks.reset();
      return std::nullopt;
    }

    const MouseEvent& mouse = event.mouse;
    if (mouse.kind == MouseEventKind::DoubleClick &&
        isMouseButtonDown(mouse, MouseButton::Left)) {
      return interaction.entryClicks.consumeDoubleClickAnchor();
    }
    if (mouse.kind == MouseEventKind::Press ||
        mouse.kind == MouseEventKind::VerticalWheel ||
        mouse.kind == MouseEventKind::HorizontalWheel) {
      interaction.entryClicks.reset();
    }
    return std::nullopt;
  }

  void handlePointerLeave() {
    const bool pointerVisualChanged =
        browser.hovered != -1 || interaction.breadcrumbHover != -1 ||
        interaction.actionHover != -1 || interaction.searchBarHover;
    browser.hovered = -1;
    interaction.breadcrumbHover = -1;
    interaction.actionHover = -1;
    interaction.searchBarHover = false;
    interaction.pointer.cancelPress();
    if (pointerVisualChanged) {
      result.dirty = true;
    }
  }

  void handleAction(const InputEvent& ev) {
    const bool browserBackAction = ev.action == InputAction::Back;
    const bool browserForwardAction = ev.action == InputAction::Forward;

    // Prefer browser navigation when the browser UI is active so that
    // mouse/browser back/forward buttons always control the browser and do
    // not inadvertently trigger playback shortcuts (ExitPlaybackSession, etc.).
    if (capabilities.interactionEnabled) {
      if (browserBackAction) {
        navigator.back();
        return;
      }
      if (browserForwardAction) {
        navigator.forward();
        return;
      }
    }

    // If browser interaction isn't active (or action wasn't handled by the
    // browser), fall back to handling playback shortcuts as before.
    if (const std::optional<PlaybackInputMatch> match =
            (capabilities.playMode || capabilities.decoderReady)
                ? matchPlaybackInput(ev, kPlaybackShortcutContextShared |
                                             kPlaybackShortcutContextGlobal)
                : std::nullopt) {
      publishPlaybackCommand(result.commands, match->command);
      result.dirty = true;
      return;
    }
  }

  void handleKey(const InputEvent& ev) {
    const KeyEvent& key = ev.key;
    if (const std::optional<PlaybackInputMatch> global =
            matchPlaybackInput(ev, kPlaybackShortcutContextGlobal)) {
      publishPlaybackCommand(result.commands, global->command);
      result.dirty = true;
      return;
    }

    if (capabilities.interactionEnabled && browserSearchFocused(browser)) {
      handleFocusedSearchKey(key);
      return;
    }

    if (capabilities.interactionEnabled) {
      const auto searchAction = browser_input::resolveKeyAction(
          key, browser_input::shortcutContext(
                   browser_input::ShortcutContext::SearchActivation));
      if (searchAction) {
        executeKeyAction(*searchAction);
        return;
      }
    }

    if (const std::optional<PlaybackInputMatch> match =
            (capabilities.playMode || capabilities.decoderReady)
                ? matchPlaybackInput(ev, kPlaybackShortcutContextShared)
                : std::nullopt) {
      publishPlaybackCommand(result.commands, match->command);
      result.dirty = true;
      return;
    }

    const auto applicationAction = browser_input::resolveKeyAction(
        key, browser_input::shortcutContext(
                 browser_input::ShortcutContext::Application));
    if (applicationAction) {
      executeKeyAction(*applicationAction);
      return;
    }
    if (!capabilities.interactionEnabled) {
      return;
    }

    const auto navigationAction = browser_input::resolveKeyAction(
        key, browser_input::shortcutContext(
                 browser_input::ShortcutContext::Navigation));
    if (navigationAction) {
      executeKeyAction(*navigationAction);
    }
  }

  void handleFocusedSearchKey(const KeyEvent& key) {
    applySearchUpdate(handleBrowserSearchKey(browser, key));
  }

  void executeKeyAction(browser_input::KeyAction action) {
    using browser_input::KeyAction;
    switch (action) {
      case KeyAction::BeginPathSearch:
        result.dirty = beginBrowserPathSearch(browser) || result.dirty;
        return;
      case KeyAction::BeginFilter:
        result.dirty = beginBrowserFilter(browser) || result.dirty;
        return;
      case KeyAction::TogglePitchMonitor:
        publishPlaybackCommand(result.commands,
                               PlaybackAction::TogglePitchMonitor);
        result.dirty = true;
        return;
      case KeyAction::ToggleSortDirection:
        browser.sortDescending = !browser.sortDescending;
        navigator.reload();
        result.dirty = true;
        return;
      case KeyAction::CycleSort: {
        int next = static_cast<int>(browser.sortMode) + 1;
        if (next > static_cast<int>(BrowserState::SortMode::Size)) next = 0;
        browser.sortMode = static_cast<BrowserState::SortMode>(next);
        browser.sortDescending =
            browser.sortMode != BrowserState::SortMode::Name;
        navigator.reload();
        result.dirty = true;
        return;
      }
      case KeyAction::Stop:
        publishPlaybackCommand(result.commands, PlaybackAction::Stop);
        result.dirty = true;
        return;
      case KeyAction::NavigateUp:
        navigateUp();
        return;
      case KeyAction::ActivateSelection:
        activateSelected(false);
        return;
      case KeyAction::OpenSelectionMenu:
        activateSelected(true);
        return;
      case KeyAction::CycleView:
        browser.viewMode = nextViewMode(browser.viewMode);
        result.dirty = true;
        return;
      case KeyAction::MoveLeft:
        moveSelection(browser, inputLayout.entries, -1, 0, result.dirty);
        return;
      case KeyAction::MoveRight:
        moveSelection(browser, inputLayout.entries, 1, 0, result.dirty);
        return;
      case KeyAction::MoveUp:
        moveSelection(browser, inputLayout.entries, 0, -1, result.dirty);
        return;
      case KeyAction::MoveDown:
        moveSelection(browser, inputLayout.entries, 0, 1, result.dirty);
        return;
      case KeyAction::PageUp:
        pageSelection(browser, inputLayout.entries, -1, result.dirty);
        return;
      case KeyAction::PageDown:
        pageSelection(browser, inputLayout.entries, 1, result.dirty);
        return;
    }
  }

  void activateSelected(bool requestContextMenu) {
    if (browser.entries.empty()) return;
    const int index = std::clamp(browser.selected, 0,
                                 static_cast<int>(browser.entries.size()) - 1);
    const BrowserEntry& entry = browser.entries[static_cast<size_t>(index)];
    if (!entry.isSelectable()) return;
    if (requestContextMenu && capabilities.playMode && entry.isMedia()) {
      result.commands.emplace_back(
          tui_input::OpenFileContextMenu{entry, -1, -1});
      result.dirty = true;
      return;
    }
    activateEntry(entry);
  }

  void applySearchUpdate(const BrowserSearchUpdate& update) {
    result.dirty = update.changed || result.dirty;
    switch (update.effect) {
      case BrowserSearchEffect::None:
        return;
      case BrowserSearchEffect::Reload:
        navigator.reload();
        return;
      case BrowserSearchEffect::Navigate:
        if (navigator.navigate(
                browserDirectoryLocation(update.navigationTarget))) {
          result.dirty = completeBrowserPathNavigation(browser) ||
                         result.dirty;
          interaction.breadcrumbHover = -1;
        }
        return;
    }
  }

  void handleMouse(
      const MouseEvent& mouse,
      const std::optional<BrowserState::EntryIdentity>& doubleClickAnchor) {
    const bool leftPressed = isMouseButtonDown(mouse, MouseButton::Left);
    const bool rightPressed = isMouseButtonDown(mouse, MouseButton::Right);
    const bool hoveredSearch =
        updatePointerHover(mouse, leftPressed, rightPressed);

    if (mouse.kind == MouseEventKind::VerticalWheel) {
      handleWheel(mouse);
      return;
    }
    if (handleBrowserChromePress(mouse, leftPressed, rightPressed,
                                 hoveredSearch)) {
      return;
    }
    if (handleActionStripPointer(mouse, leftPressed)) return;
    if (handleDragOrSeek(mouse, leftPressed)) return;
    if (!capabilities.interactionEnabled) return;
    handleEntryPointer(mouse, leftPressed, rightPressed, doubleClickAnchor);
  }

  bool updatePointerHover(const MouseEvent& mouse, bool leftPressed,
                          bool rightPressed) {
    const bool hoveredSearch = capabilities.interactionEnabled &&
                               isMouseInSearchBar(mouse, inputLayout.searchBarY,
                                                  inputLayout.searchBarWidth);
    if (browserSearchFocused(browser) &&
        capabilities.interactionEnabled &&
        (mouse.kind == MouseEventKind::Press ||
         mouse.kind == MouseEventKind::VerticalWheel) &&
        !hoveredSearch &&
        (leftPressed || rightPressed ||
         mouse.kind == MouseEventKind::VerticalWheel)) {
      applySearchUpdate(blurBrowserSearch(browser));
    }
    if (interaction.searchBarHover != hoveredSearch) {
      interaction.searchBarHover = hoveredSearch;
      result.dirty = true;
    }

    if (capabilities.interactionEnabled) {
      const int nextHover =
          breadcrumbIndexAt(inputLayout.breadcrumbs, mouse.pos.X, mouse.pos.Y,
                            inputLayout.breadcrumbY);
      if (nextHover != interaction.breadcrumbHover) {
        interaction.breadcrumbHover = nextHover;
        result.dirty = true;
      }
    } else if (interaction.breadcrumbHover != -1) {
      interaction.breadcrumbHover = -1;
      result.dirty = true;
    }

    const int nextActionHover =
        actionStripIndexAt(inputLayout.actionStrip, mouse.pos.X, mouse.pos.Y);
    if (nextActionHover != interaction.actionHover) {
      interaction.actionHover = nextActionHover;
      result.dirty = true;
    }

    const int nextEntryHover =
        capabilities.interactionEnabled
            ? browserEntryIndexAt(browser, inputLayout.entries, mouse.pos.X,
                                  mouse.pos.Y, inputLayout.listTop)
            : -1;
    if (setBrowserHoveredEntry(browser, nextEntryHover)) {
      result.dirty = true;
    }
    return hoveredSearch;
  }

  void handleWheel(const MouseEvent& mouse) {
    const int delta = mouse.wheelDelta;
    if (delta == 0) return;

    const std::optional<ActionStripItem> action = actionAtPointer(mouse);
    if (capabilities.interactionEnabled && action == ActionStripItem::View) {
      browser.viewMode = delta > 0 ? prevViewMode(browser.viewMode)
                                   : nextViewMode(browser.viewMode);
      result.dirty = true;
      return;
    }
    if (!capabilities.interactionEnabled) return;

    browser.scrollRow -= delta / WHEEL_DELTA;
    const int maxScroll = std::max(
        0, inputLayout.entries.totalRows - inputLayout.entries.rowsVisible);
    browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
    result.dirty = true;
  }

  bool handleBrowserChromePress(const MouseEvent& mouse, bool leftPressed,
                                bool rightPressed, bool hoveredSearch) {
    if (capabilities.interactionEnabled && leftPressed &&
        mouse.kind == MouseEventKind::Press &&
        interaction.breadcrumbHover >= 0) {
      const auto& crumb =
          inputLayout.breadcrumbs
              .crumbs[static_cast<size_t>(interaction.breadcrumbHover)];
      if (browser.location != crumb.location) {
        navigator.navigate(crumb.location);
        interaction.breadcrumbHover = -1;
        result.dirty = true;
      }
      return true;
    }

    if (capabilities.interactionEnabled && leftPressed &&
        mouse.kind == MouseEventKind::Press && hoveredSearch) {
      result.dirty = beginBrowserFilter(browser) || result.dirty;
      return true;
    }

    if (capabilities.interactionEnabled && rightPressed &&
        mouse.kind == MouseEventKind::Press &&
        actionAtPointer(mouse) == ActionStripItem::View) {
      browser.viewMode = prevViewMode(browser.viewMode);
      result.dirty = true;
      return true;
    }
    return false;
  }

  bool handleActionStripPointer(const MouseEvent& mouse, bool leftPressed) {
    if (mouse.kind == MouseEventKind::Press && leftPressed) {
      if (const auto action = actionAtPointer(mouse)) {
        interaction.pointer.pressAction(*action);
        return true;
      }
      interaction.pointer.cancelPress();
      return false;
    }

    if (mouse.kind == MouseEventKind::Release &&
        mouse.button == MouseButton::Left) {
      if (const auto action =
              interaction.pointer.releaseAction(actionAtPointer(mouse))) {
        invokeAction(*action);
        return true;
      }
      return false;
    }

    if (mouse.kind == MouseEventKind::Move &&
        interaction.pointer.hasPressedAction()) {
      if (leftPressed) return true;
      interaction.pointer.cancelPress();
    }
    return false;
  }

  bool handleDragOrSeek(const MouseEvent& mouse, bool leftPressed) {
    if (!leftPressed || (mouse.kind != MouseEventKind::Press &&
                         mouse.kind != MouseEventKind::Move)) {
      return false;
    }

    if (inputLayout.entries.showScrollBar &&
        inputLayout.entries.scrollBarX >= 0 &&
        inputLayout.entries.scrollBarWidth > 0 &&
        mouse.pos.X >= inputLayout.entries.scrollBarX &&
        mouse.pos.X < inputLayout.entries.scrollBarX +
                          inputLayout.entries.scrollBarWidth &&
        mouse.pos.Y >= inputLayout.listTop &&
        mouse.pos.Y < inputLayout.listTop + inputLayout.listHeight) {
      scrollFromBar(browser, inputLayout.entries, mouse.pos.Y,
                    inputLayout.listTop, inputLayout.listHeight, result.dirty);
      return true;
    }

    if (inputLayout.progressBarX < 0 || inputLayout.progressBarY < 0 ||
        inputLayout.progressBarWidth <= 0) {
      return false;
    }

    const double unitWidth =
        mouse.hasPixelPosition ? std::max(1.0, mouse.unitWidth) : 1.0;
    const double unitHeight =
        mouse.hasPixelPosition ? std::max(1.0, mouse.unitHeight) : 1.0;
    const playback_overlay::ProgressBarRegion progressRegion{
        {inputLayout.progressBarX * unitWidth,
         inputLayout.progressBarY * unitHeight,
         (inputLayout.progressBarX + inputLayout.progressBarWidth) * unitWidth,
         (inputLayout.progressBarY + 1) * unitHeight},
        inputLayout.progressBarWidth};
    const double pointerX = mouse.hasPixelPosition ? mouse.pixelX : mouse.pos.X;
    const double pointerY = mouse.hasPixelPosition ? mouse.pixelY : mouse.pos.Y;
    const auto hit =
        playback_overlay::progressBarHitAt(progressRegion, pointerX, pointerY);
    if (!hit) return false;

    publishPlaybackCommand(result.commands,
                           playback_input::SeekToRatio{hit->ratio});
    result.dirty = true;
    return true;
  }

  void handleEntryPointer(
      const MouseEvent& mouse, bool leftPressed, bool rightPressed,
      const std::optional<BrowserState::EntryIdentity>& doubleClickAnchor) {
    const int count = static_cast<int>(browser.entries.size());
    if (count == 0) return;

    const int index =
        browserEntryIndexAt(browser, inputLayout.entries, mouse.pos.X,
                            mouse.pos.Y, inputLayout.listTop);
    if (index < 0 || index >= count) return;

    const BrowserEntry& entry = browser.entries[static_cast<size_t>(index)];
    if (!entry.isSelectable()) return;

    if (mouse.kind == MouseEventKind::Press && rightPressed) {
      if (browser.selected != index) {
        browser.selected = index;
        result.dirty = true;
      }
      if (entry.isMedia()) {
        result.commands.emplace_back(
            tui_input::OpenFileContextMenu{entry, mouse.pos.X, mouse.pos.Y});
        result.dirty = true;
      }
      return;
    }

    if (!leftPressed || (mouse.kind != MouseEventKind::Press &&
                         mouse.kind != MouseEventKind::DoubleClick)) {
      return;
    }
    if (browser.selected != index) {
      browser.selected = index;
      result.dirty = true;
    }
    if (mouse.kind == MouseEventKind::Press) {
      interaction.entryClicks.recordPress(entry);
    } else if (browser_input::pointerActivatesEntry(mouse.kind) &&
               doubleClickAnchor &&
               browserEntryMatchesIdentity(entry, *doubleClickAnchor)) {
      activateEntry(entry);
    }
  }

  std::optional<ActionStripItem> actionAtPointer(
      const MouseEvent& mouse) const {
    const int actionIndex =
        actionStripIndexAt(inputLayout.actionStrip, mouse.pos.X, mouse.pos.Y);
    if (actionIndex < 0) return std::nullopt;
    return inputLayout.actionStrip.buttons[static_cast<size_t>(actionIndex)].id;
  }

  void invokeAction(ActionStripItem action) {
    switch (action) {
      case ActionStripItem::Previous:
        publishPlaybackCommand(result.commands, PlaybackAction::Previous);
        break;
      case ActionStripItem::PlayPause:
        publishPlaybackCommand(result.commands, PlaybackAction::TogglePause);
        break;
      case ActionStripItem::Next:
        publishPlaybackCommand(result.commands, PlaybackAction::Next);
        break;
      case ActionStripItem::Radio:
        publishPlaybackCommand(result.commands, PlaybackAction::ToggleRadio);
        break;
      case ActionStripItem::Hz50:
        publishPlaybackCommand(result.commands, PlaybackAction::Toggle50Hz);
        break;
      case ActionStripItem::PitchMonitor:
        publishPlaybackCommand(result.commands,
                               PlaybackAction::TogglePitchMonitor);
        break;
      case ActionStripItem::View:
        browser.viewMode = nextViewMode(browser.viewMode);
        break;
      case ActionStripItem::Options:
        publishPlaybackCommand(result.commands, PlaybackAction::ToggleOptions);
        break;
      case ActionStripItem::PictureInPicture:
        publishPlaybackCommand(result.commands,
                               PlaybackAction::TogglePictureInPicture);
        break;
    }
    result.dirty = true;
  }

  bool navigateDirectoryWithHistory(const std::filesystem::path& dir) {
    const bool navigated = navigator.navigate(browserDirectoryLocation(dir));
    if (navigated) interaction.breadcrumbHover = -1;
    return navigated;
  }

  bool navigateUp() {
    if (optionsBrowserIsActive(browser)) {
      const std::optional<BrowserLocation> parent =
          browserOptionsParentLocation(browser.location);
      const bool navigated =
          parent ? navigator.navigate(*parent) : navigator.closeContext();
      if (navigated) interaction.breadcrumbHover = -1;
      return navigated;
    }

    const bool leavingTrackBrowser = isTrackBrowserActive(browser);
    const std::filesystem::path departedPath = browser.location.path();
    const std::optional<std::filesystem::path> parent =
        browserParentDirectory(departedPath);
    if (!parent) return false;

    BrowserEntry departedEntry{
        toUtf8String(departedPath.filename()), departedPath,
        leavingTrackBrowser
            ? browser_entry::Action{browser_entry::OpenFile{}}
            : browser_entry::Action{browser_entry::OpenDirectory{}}};
    const BrowserState::EntryIdentity departed =
        browserEntryIdentity(departedEntry);

    const bool navigated =
        navigator.navigate(browserDirectoryLocation(*parent), {}, departed);
    if (navigated) interaction.breadcrumbHover = -1;
    return navigated;
  }

  void activateEntry(const BrowserEntry& entry) {
    if (entry.actionAs<browser_entry::NavigateUp>()) {
      navigateUp();
      return;
    }
    if (entry.actionAs<browser_entry::OpenDirectory>()) {
      navigateDirectoryWithHistory(entry.path);
      return;
    }
    if (const auto* location = entry.actionAs<browser_entry::OpenLocation>()) {
      navigator.navigate(location->target);
      return;
    }
    if (!entry.isActivatable()) return;
    if (capabilities.playMode) {
      result.commands.emplace_back(tui_input::ActivateEntry{entry});
      result.dirty = true;
      return;
    }
    if (entry.isMedia()) {
      result.commands.emplace_back(tui_input::RenderFile{entry.path});
      result.quitRequested = true;
    }
  }

  BrowserNavigator& navigator;
  BrowserState& browser;
  BrowserInteractionState& interaction;
  const BrowserInputLayout& inputLayout;
  const BrowserInputCapabilities& capabilities;
  BrowserInputResult result;
};

}  // namespace

BrowserInputResult handleInputEvent(
    const InputEvent& event, BrowserNavigator& navigator,
    BrowserInteractionState& interaction, const BrowserInputLayout& layout,
    const BrowserInputCapabilities& capabilities) {
  return BrowserInputController(navigator, interaction, layout, capabilities)
      .handle(event);
}
