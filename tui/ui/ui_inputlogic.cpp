#include "ui_inputlogic.h"

#include <algorithm>
#include <cstdlib>
#include <optional>

#include "browser_navigation.h"
#include "browser_grid_index.h"
#include "consolescreen.h"
#include "optionsbrowser.h"
#include "playback/input/shortcuts.h"
#include "playback/overlay/interaction.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"
#include "ui_helpers.h"

namespace {
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
}  // namespace

void setBrowserSearchFocus(BrowserState& browser, BrowserSearchFocus focus,
                          bool& dirty) {
  if (focus == BrowserSearchFocus::None) {
    if (!browser.filterActive && !browser.pathSearchActive) return;
    browser.filterActive = false;
    browser.pathSearchActive = false;
    browser.pathSearch.clear();
    dirty = true;
    return;
  }

  if (focus == BrowserSearchFocus::Filter) {
    const bool previouslyFocused = browser.filterActive;
    const bool previouslyPathSearching = browser.pathSearchActive;
    browser.filterActive = true;
    if (browser.pathSearchActive) {
      browser.pathSearchActive = false;
      browser.pathSearch.clear();
    }
    if (!previouslyFocused || previouslyPathSearching) {
      dirty = true;
    }
    return;
  }

  const bool previouslyFocused = browser.filterActive;
  const bool previouslyPathSearching = browser.pathSearchActive;
  browser.pathSearchActive = true;
  browser.filterActive = false;
  if (!previouslyPathSearching || previouslyFocused) {
    dirty = true;
  }
}

void handleInputEvent(const InputEvent& ev, BrowserNavigator& navigator,
                      const GridLayout& layout,
                      const BreadcrumbLine& breadcrumbLine, int breadcrumbY,
                      int searchBarY, int searchBarWidth, int listTop,
                      int listHeight, int progressBarX,
                      int progressBarY, int progressBarWidth,
                      const ActionStripLayout& actionStrip,
                      bool browserInteractionEnabled, bool playMode,
                      bool decoderReady, int& breadcrumbHover, int& actionHover,
                      bool& searchBarHover, bool& dirty, bool& running,
                      const InputCallbacks& callbacks) {
  BrowserState& browser = navigator.state();
  auto navigateDirectoryWithHistory = [&](const std::filesystem::path& dir) {
    const bool navigated = navigator.navigate(browserDirectoryLocation(dir));
    if (navigated) breadcrumbHover = -1;
    return navigated;
  };
  auto navigateUp = [&]() {
    if (optionsBrowserIsActive(browser)) {
      const std::optional<BrowserLocation> parent =
          browserOptionsParentLocation(browser.location);
      const bool navigated = parent ? navigator.navigate(*parent)
                                    : navigator.closeContext();
      if (navigated) breadcrumbHover = -1;
      return navigated;
    }

    const bool leavingTrackBrowser = isTrackBrowserActive(browser);
    const std::filesystem::path departedPath = browser.location.path;
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

    const bool navigated = navigator.navigate(browserDirectoryLocation(*parent),
                                               {}, departed);
    if (navigated) breadcrumbHover = -1;
    return navigated;
  };
  auto activateEntry = [&](const BrowserEntry& entry) {
    if (entry.actionAs<browser_entry::NavigateUp>()) {
      navigateUp();
      return;
    }
    if (entry.actionAs<browser_entry::OpenDirectory>()) {
      navigateDirectoryWithHistory(entry.path);
      return;
    }
    if (const auto* location =
            entry.actionAs<browser_entry::OpenLocation>()) {
      navigator.navigate(location->target);
      return;
    }
    if (!entry.isActivatable()) {
      return;
    }
    if (playMode) {
      if (callbacks.onActivateEntry && callbacks.onActivateEntry(entry)) {
        dirty = true;
      }
      return;
    }
    if (entry.isMedia()) {
      if (callbacks.onRenderFile) {
        callbacks.onRenderFile(entry.path);
      }
      running = false;
    }
  };

  auto resolvePathSearchTarget = [&](const std::string& query,
                                    std::filesystem::path& out) -> bool {
    if (query.empty()) return false;
    std::string text = query;
    if (!text.empty() && text[0] == '~') {
      std::string home;
      if (const auto envProfile = getEnvString("USERPROFILE")) {
        home = *envProfile;
      }
      if (home.empty()) {
        const auto homeDrive = getEnvString("HOMEDRIVE");
        const auto homePath = getEnvString("HOMEPATH");
        if (homeDrive && !homeDrive->empty() && homePath &&
            !homePath->empty()) {
          home = *homeDrive + *homePath;
        }
      }
      if (!home.empty()) {
        if (text == "~") {
          text = home;
        } else if (text.size() > 1 && (text[1] == '/' || text[1] == '\\')) {
          std::string rest = text.substr(2);
          std::filesystem::path homePath(home);
          homePath /= rest;
          text = toUtf8String(homePath);
        } else {
          std::filesystem::path homePath(home);
          homePath /= text.substr(1);
          text = toUtf8String(homePath);
        }
      }
    }

    std::filesystem::path target(text);
    if (target.has_root_name() && !target.has_root_directory() &&
        target.relative_path().empty()) {
      target = target.root_name();
      target /= std::filesystem::path();
    }
    if (!target.is_absolute()) {
      const std::filesystem::path base =
          browser.location.kind == BrowserLocationKind::Directory
              ? browser.location.path
              : browser.location.path.parent_path();
      target = base / target;
    }

    if (!target.has_root_name() && !target.has_root_directory() &&
        target.relative_path().empty()) {
      return false;
    }

    std::error_code existsEc;
    if (!std::filesystem::exists(target, existsEc)) return false;
    std::error_code dirEc;
    if (!std::filesystem::is_directory(target, dirEc)) return false;
    out = target;
    return true;
  };

  auto commitPathSearch = [&]() {
    std::filesystem::path target;
    if (!resolvePathSearchTarget(browser.pathSearch, target)) return false;
    if (navigator.navigate(browserDirectoryLocation(target))) {
      setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
      breadcrumbHover = -1;
      dirty = true;
      return true;
    }
    return false;
  };

  if (ev.type == InputEvent::Type::Resize) {
    dirty = true;
    if (callbacks.onResize) callbacks.onResize();
    return;
  }

  const bool browserBackAction =
      ev.type == InputEvent::Type::Action && ev.action == InputAction::Back;
  const bool browserForwardAction =
      ev.type == InputEvent::Type::Action &&
      ev.action == InputAction::Forward;

  if (ev.type == InputEvent::Type::Action) {
    // Prefer browser navigation when the browser UI is active so that
    // mouse/browser back/forward buttons always control the browser and do
    // not inadvertently trigger playback shortcuts (ExitPlaybackSession, etc.).
    if (browserInteractionEnabled) {
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
    if ((playMode || decoderReady) &&
        handlePlaybackInput(ev, callbacks,
                            kPlaybackShortcutContextShared |
                                kPlaybackShortcutContextGlobal) !=
            PlaybackInputResult::Ignored) {
      dirty = true;
      return;
    }
    return;
  }

  if (ev.type == InputEvent::Type::Key) {
    const KeyEvent& key = ev.key;
    bool backspaceKey = key.vk == VK_BACK;
    const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
    bool ctrl = (key.control & ctrlMask) != 0;

    if (ctrl && (key.vk == 'Q' || key.ch == 'q' || key.ch == 'Q')) {
      if (callbacks.onQuit) callbacks.onQuit();
      else running = false;
      dirty = true;
      return;
    }

    if (browserInteractionEnabled && browser.pathSearchActive) {
      if (key.vk == VK_ESCAPE) {
        setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
        dirty = true;
        return;
      }
      if (key.vk == VK_RETURN) {
        commitPathSearch();
        return;
      }
      if (backspaceKey) {
        if (!browser.pathSearch.empty()) {
          browser.pathSearch.pop_back();
          dirty = true;
        }
        return;
      }
      if (key.ch >= 32) {
        browser.pathSearch.push_back(key.ch);
        dirty = true;
        return;
      }
      return;
    }

    if (browserInteractionEnabled && browser.filterActive) {
      if (key.vk == VK_ESCAPE) {
        browser.filter = browser.filterBackup;
        setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
        navigator.reload();
        return;
      }
      if (key.vk == VK_RETURN) {
        setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
        navigator.reload();
        return;
      }
      if (backspaceKey) {
        if (!browser.filter.empty()) {
          browser.filter.pop_back();
          dirty = true;
        }
        return;
      }
      if (key.ch >= 32) {
        browser.filter += key.ch;
        dirty = true;
        return;
      }
      return;
    }

    if (browserInteractionEnabled && ctrl &&
        (key.vk == 'G' || key.ch == 'g' || key.ch == 'G')) {
      setBrowserSearchFocus(browser, BrowserSearchFocus::PathSearch, dirty);
      browser.pathSearch.clear();
      dirty = true;
      return;
    }
    if (browserInteractionEnabled && ctrl &&
        (key.vk == 'F' || key.ch == 'f' || key.ch == 'F')) {
      browser.filterBackup = browser.filter;
      setBrowserSearchFocus(browser, BrowserSearchFocus::Filter, dirty);
      browser.pathSearch.clear();
      dirty = true;
      return;
    }
    if (browserInteractionEnabled && (key.vk == VK_DIVIDE || key.ch == '/')) {
      browser.filterBackup = browser.filter;
      setBrowserSearchFocus(browser, BrowserSearchFocus::Filter, dirty);
      browser.pathSearch.clear();
      dirty = true;
      return;
    }

    if ((playMode || decoderReady) &&
        handlePlaybackInput(ev, callbacks,
                            kPlaybackShortcutContextShared |
                                kPlaybackShortcutContextGlobal) !=
            PlaybackInputResult::Ignored) {
      dirty = true;
      return;
    }
    if (!browserInteractionEnabled) {
      return;
    }
    if (key.vk == 'S' || key.ch == 's' || key.ch == 'S') {
      const DWORD sortAltMask = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
      bool sortAlt = (key.control & sortAltMask) != 0;
      if (sortAlt) {
        browser.sortDescending = !browser.sortDescending;
      } else {
        int next = static_cast<int>(browser.sortMode) + 1;
        if (next > static_cast<int>(BrowserState::SortMode::Size)) next = 0;
        browser.sortMode = static_cast<BrowserState::SortMode>(next);

        if (browser.sortMode == BrowserState::SortMode::Name)
          browser.sortDescending = false;
        else
          browser.sortDescending = true;
      }
      navigator.reload();
      dirty = true;
      return;
    }
    if (key.vk == VK_ESCAPE) {
      if (callbacks.onStopPlayback) {
        callbacks.onStopPlayback();
        dirty = true;
      }
      return;
    }
    if (backspaceKey) {
      navigateUp();
      return;
    }
    if (key.vk == VK_RETURN) {
      int count = static_cast<int>(browser.entries.size());
      if (count > 0) {
        const auto& pick = browser.entries[static_cast<size_t>(browser.selected)];
        if (!pick.isSelectable()) return;
        if (ctrl && playMode && pick.isMedia()) {
          if (callbacks.onOpenFileContextMenu) {
            callbacks.onOpenFileContextMenu(pick, -1, -1);
            dirty = true;
          }
          return;
        }
        activateEntry(pick);
      }
      return;
    }
    if (key.vk == 'T' || key.ch == 't' || key.ch == 'T') {
      browser.viewMode = nextViewMode(browser.viewMode);
      dirty = true;
      return;
    }
    if (key.vk == VK_LEFT) {
      moveSelection(browser, layout, -1, 0, dirty);
      return;
    }
    if (key.vk == VK_RIGHT) {
      moveSelection(browser, layout, 1, 0, dirty);
      return;
    }
    if (key.vk == VK_UP) {
      moveSelection(browser, layout, 0, -1, dirty);
      return;
    }
    if (key.vk == VK_DOWN) {
      moveSelection(browser, layout, 0, 1, dirty);
      return;
    }
    if (key.vk == VK_PRIOR) {
      pageSelection(browser, layout, -1, dirty);
      return;
    }
    if (key.vk == VK_NEXT) {
      pageSelection(browser, layout, 1, dirty);
      return;
    }
  }

  if (ev.type == InputEvent::Type::Mouse) {
    const MouseEvent& mouse = ev.mouse;
    bool leftPressed = (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
    bool rightPressed = (mouse.buttonState & RIGHTMOST_BUTTON_PRESSED) != 0;
    bool hoveredSearch = browserInteractionEnabled &&
                         isMouseInSearchBar(mouse, searchBarY, searchBarWidth);
    if ((browser.filterActive || browser.pathSearchActive) &&
        browserInteractionEnabled &&
        (mouse.eventFlags == 0 || mouse.eventFlags == MOUSE_WHEELED) &&
        !hoveredSearch &&
        (leftPressed || rightPressed || mouse.eventFlags == MOUSE_WHEELED)) {
      setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
    }
    if (searchBarHover != hoveredSearch) {
      searchBarHover = hoveredSearch;
      dirty = true;
    }
    if (browserInteractionEnabled) {
      int nextHover =
          breadcrumbIndexAt(breadcrumbLine, mouse.pos.X, mouse.pos.Y, breadcrumbY);
      if (nextHover != breadcrumbHover) {
        breadcrumbHover = nextHover;
        dirty = true;
      }
    } else if (breadcrumbHover != -1) {
      breadcrumbHover = -1;
      dirty = true;
    }
    int nextActionHover = actionStripIndexAt(actionStrip, mouse.pos.X, mouse.pos.Y);
    if (nextActionHover != actionHover) {
      actionHover = nextActionHover;
      dirty = true;
    }
    if (mouse.eventFlags == MOUSE_WHEELED) {
      int delta = static_cast<SHORT>(HIWORD(mouse.buttonState));
      if (delta != 0) {
        int actionIndex = actionStripIndexAt(actionStrip, mouse.pos.X, mouse.pos.Y);
        if (actionIndex >= 0) {
          const auto& btn = actionStrip.buttons[static_cast<size_t>(actionIndex)];
          if (browserInteractionEnabled && btn.id == ActionStripItem::View) {
            browser.viewMode = (delta > 0) ? prevViewMode(browser.viewMode)
                                           : nextViewMode(browser.viewMode);
            dirty = true;
            return;
          }
        }
        if (!browserInteractionEnabled) {
          return;
        }
        browser.scrollRow -= delta / WHEEL_DELTA;
        int maxScroll = std::max(0, layout.totalRows - layout.rowsVisible);
        browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
        dirty = true;
      }
      return;
    }

    if (browserInteractionEnabled && leftPressed && mouse.eventFlags == 0 &&
        breadcrumbHover >= 0) {
      const auto& crumb =
          breadcrumbLine.crumbs[static_cast<size_t>(breadcrumbHover)];
      if (browser.location != crumb.location) {
        navigator.navigate(crumb.location);
        breadcrumbHover = -1;
        dirty = true;
      }
      return;
    }

    if (browserInteractionEnabled && leftPressed && mouse.eventFlags == 0 &&
        hoveredSearch) {
      browser.filterBackup = browser.filter;
      setBrowserSearchFocus(browser, BrowserSearchFocus::Filter, dirty);
      dirty = true;
      return;
    }

    if (browserInteractionEnabled && rightPressed && mouse.eventFlags == 0) {
      int actionIndex = actionStripIndexAt(actionStrip, mouse.pos.X, mouse.pos.Y);
      if (actionIndex >= 0) {
        const auto& btn = actionStrip.buttons[static_cast<size_t>(actionIndex)];
        if (btn.id == ActionStripItem::View) {
          browser.viewMode = prevViewMode(browser.viewMode);
          dirty = true;
          return;
        }
      }
    }

    if (leftPressed && (mouse.eventFlags == 0 || mouse.eventFlags == MOUSE_MOVED)) {
      int actionIndex = actionStripIndexAt(actionStrip, mouse.pos.X, mouse.pos.Y);
      if (actionIndex >= 0) {
        const auto& btn = actionStrip.buttons[static_cast<size_t>(actionIndex)];
        switch (btn.id) {
          case ActionStripItem::Previous:
            if (callbacks.onPlayPrevious) callbacks.onPlayPrevious();
            dirty = true;
            return;
          case ActionStripItem::PlayPause:
            if (callbacks.onTogglePause) callbacks.onTogglePause();
            dirty = true;
            return;
          case ActionStripItem::Next:
            if (callbacks.onPlayNext) callbacks.onPlayNext();
            dirty = true;
            return;
          case ActionStripItem::Radio:
            if (callbacks.onToggleRadio) callbacks.onToggleRadio();
            dirty = true;
            return;
          case ActionStripItem::Hz50:
            if (callbacks.onToggle50Hz) callbacks.onToggle50Hz();
            dirty = true;
            return;
          case ActionStripItem::View:
            browser.viewMode = nextViewMode(browser.viewMode);
            dirty = true;
            return;
          case ActionStripItem::Options:
            if (callbacks.onToggleOptions) callbacks.onToggleOptions();
            dirty = true;
            return;
          case ActionStripItem::PictureInPicture:
            if (callbacks.onToggleWindow) callbacks.onToggleWindow();
            dirty = true;
            return;
        }
      }
      if (layout.showScrollBar && layout.scrollBarX >= 0 &&
          layout.scrollBarWidth > 0 && mouse.pos.X >= layout.scrollBarX &&
          mouse.pos.X < layout.scrollBarX + layout.scrollBarWidth &&
          mouse.pos.Y >= listTop && mouse.pos.Y < listTop + listHeight) {
        scrollFromBar(browser, layout, mouse.pos.Y, listTop, listHeight, dirty);
        return;
      }
      const double unitWidth =
          mouse.hasPixelPosition ? std::max(1.0, mouse.unitWidth) : 1.0;
      const double unitHeight =
          mouse.hasPixelPosition ? std::max(1.0, mouse.unitHeight) : 1.0;
      if (progressBarX >= 0 && progressBarY >= 0 && progressBarWidth > 0) {
        const playback_overlay::ProgressBarRegion progressRegion{
            {progressBarX * unitWidth, progressBarY * unitHeight,
             (progressBarX + progressBarWidth) * unitWidth,
             (progressBarY + 1) * unitHeight},
            progressBarWidth};
        const double pointerX =
            mouse.hasPixelPosition ? mouse.pixelX : mouse.pos.X;
        const double pointerY =
            mouse.hasPixelPosition ? mouse.pixelY : mouse.pos.Y;
        if (const auto hit = playback_overlay::progressBarHitAt(
                progressRegion, pointerX, pointerY)) {
          if (callbacks.onSeekToRatio) callbacks.onSeekToRatio(hit->ratio);
          dirty = true;
          return;
        }
      }
    }
    if (!browserInteractionEnabled) {
      return;
    }

    int count = static_cast<int>(browser.entries.size());
    if (count == 0) return;
    int x = mouse.pos.X;
    int y = mouse.pos.Y;
    int cellHeight = std::max(1, layout.cellHeight);
    int visibleHeight = layout.rowsVisible * cellHeight;
    if (y < listTop || y >= listTop + visibleHeight) return;
    int row = (y - listTop) / cellHeight + browser.scrollRow;
    int col = layout.colWidth > 0 ? x / layout.colWidth : 0;
    if (col < 0 || col >= layout.cols) return;
    int idx =
        browserGridEntryIndex(layout, browser.viewMode, row, col, count);
    if (idx < 0 || idx >= count) return;

    if (mouse.eventFlags == MOUSE_MOVED && !leftPressed) {
      if (!browser.entries[static_cast<size_t>(idx)].isSelectable()) {
        return;
      }
      if (browser.selected != idx) {
        browser.selected = idx;
        dirty = true;
      }
      return;
    }

    if (mouse.eventFlags == 0 && rightPressed) {
      if (!browser.entries[static_cast<size_t>(idx)].isSelectable()) {
        return;
      }
      if (browser.selected != idx) {
        browser.selected = idx;
        dirty = true;
      }
      const auto& pick = browser.entries[static_cast<size_t>(browser.selected)];
      if (pick.isMedia() && callbacks.onOpenFileContextMenu) {
        callbacks.onOpenFileContextMenu(pick, mouse.pos.X, mouse.pos.Y);
        dirty = true;
      }
      return;
    }

    if (mouse.eventFlags == 0 && leftPressed) {
      if (!browser.entries[static_cast<size_t>(idx)].isSelectable()) {
        return;
      }
      if (browser.selected != idx) {
        browser.selected = idx;
        dirty = true;
      }
      const auto& pick = browser.entries[static_cast<size_t>(browser.selected)];
      activateEntry(pick);
    }
  }
}

PlaybackInputResult handlePlaybackInput(const InputEvent& ev,
                                        const InputCallbacks& callbacks,
                                        uint32_t shortcutContexts) {
  if (auto action = resolvePlaybackShortcutAction(ev, shortcutContexts)) {
    switch (*action) {
      case PlaybackShortcutAction::Quit:
        if (callbacks.onQuit) callbacks.onQuit();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::Play:
        if (callbacks.onPlay) callbacks.onPlay();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::Pause:
        if (callbacks.onPause) callbacks.onPause();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::TogglePause:
        if (callbacks.onTogglePause) callbacks.onTogglePause();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::Stop:
        if (callbacks.onStopPlayback) callbacks.onStopPlayback();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::Previous:
        if (callbacks.onPlayPrevious) callbacks.onPlayPrevious();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::Next:
        if (callbacks.onPlayNext) callbacks.onPlayNext();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::ToggleWindow:
        if (callbacks.onToggleWindow) callbacks.onToggleWindow();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::ToggleFullscreen:
        if (callbacks.onToggleFullscreen) callbacks.onToggleFullscreen();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::ToggleRadio:
        if (callbacks.onToggleRadio) callbacks.onToggleRadio();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::Toggle50Hz:
        if (callbacks.onToggle50Hz) callbacks.onToggle50Hz();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::ToggleSubtitles:
        if (callbacks.onToggleSubtitles) callbacks.onToggleSubtitles();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::ToggleAudioTrack:
        if (callbacks.onToggleAudioTrack) callbacks.onToggleAudioTrack();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::ToggleOptions:
        if (callbacks.onToggleOptions) callbacks.onToggleOptions();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::SeekBackward:
        if (callbacks.onSeekBy) callbacks.onSeekBy(-1);
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::SeekForward:
        if (callbacks.onSeekBy) callbacks.onSeekBy(1);
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::PreviousFrame:
        if (callbacks.onPreviousFrame) callbacks.onPreviousFrame();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::NextFrame:
        if (callbacks.onNextFrame) callbacks.onNextFrame();
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::CopyVideoFrame:
        if (callbacks.onCopyVideoFrame) callbacks.onCopyVideoFrame();
        return PlaybackInputResult::HandledWithoutOverlayRefresh;
      case PlaybackShortcutAction::VolumeUp:
        if (callbacks.onAdjustVolume) callbacks.onAdjustVolume(0.10f);
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::VolumeDown:
        if (callbacks.onAdjustVolume) callbacks.onAdjustVolume(-0.10f);
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::TogglePictureInPicture:
      case PlaybackShortcutAction::OpenVideoEditor:
      case PlaybackShortcutAction::RequestCloseVideoEditor:
      case PlaybackShortcutAction::NavigateBackInVideoEditor:
      case PlaybackShortcutAction::ConfirmVideoEditPrompt:
      case PlaybackShortcutAction::SetVideoEditIn:
      case PlaybackShortcutAction::SetVideoEditOut:
      case PlaybackShortcutAction::ClearVideoEditIn:
      case PlaybackShortcutAction::ClearVideoEditOut:
      case PlaybackShortcutAction::ClearVideoEditInAndOut:
      case PlaybackShortcutAction::RippleDeleteVideoEditSelection:
      case PlaybackShortcutAction::TrimVideoEditSelection:
      case PlaybackShortcutAction::UndoVideoEdit:
      case PlaybackShortcutAction::RedoVideoEdit:
      case PlaybackShortcutAction::ResetVideoEdits:
      case PlaybackShortcutAction::ExportVideoEdits:
      case PlaybackShortcutAction::DiscardVideoEditsAndExit:
      case PlaybackShortcutAction::CancelVideoEditPrompt:
        if (callbacks.onPlaybackContextShortcut) {
          callbacks.onPlaybackContextShortcut(*action);
        }
        return PlaybackInputResult::Handled;
      case PlaybackShortcutAction::ExitPlaybackSession:
      case PlaybackShortcutAction::DismissPictureInPicture:
      case PlaybackShortcutAction::CloseViewer:
        if (callbacks.onPlaybackContextShortcut) {
          callbacks.onPlaybackContextShortcut(*action);
        }
        return PlaybackInputResult::HandledWithoutOverlayRefresh;
    }
  }
  return PlaybackInputResult::Ignored;
}
