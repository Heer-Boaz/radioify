#include "consoleinput.h"

#include <algorithm>
#include <cstdlib>

#include "browser_grid_index.h"
#include "consolescreen.h"
#include "optionsbrowser.h"
#include "playback/playback_media_keys.h"
#include "runtime_helpers.h"
#include "ui_helpers.h"

namespace {
bool isSelectableEntry(const FileEntry& entry) {
  return !entry.isSectionHeader;
}

int nearestSelectableEntryAny(const std::vector<FileEntry>& entries, int start) {
  if (entries.empty()) return -1;
  int n = static_cast<int>(entries.size());
  int idx = std::clamp(start, 0, n - 1);
  if (isSelectableEntry(entries[static_cast<size_t>(idx)])) return idx;
  for (int delta = 1; delta < n; ++delta) {
    int forward = idx + delta;
    if (forward >= n) forward -= n;
    if (isSelectableEntry(entries[static_cast<size_t>(forward)])) return forward;

    int backward = idx - delta;
    if (backward < 0) backward += n;
    if (isSelectableEntry(entries[static_cast<size_t>(backward)])) return backward;
  }
  return -1;
}

bool isMouseInSearchBar(const MouseEvent& mouse, int searchBarY,
                       int searchBarWidth) {
  return searchBarY >= 0 && searchBarWidth > 0 && mouse.pos.Y == searchBarY &&
         mouse.pos.X >= 0 && mouse.pos.X < searchBarWidth;
}

int nearestSelectableEntry(const std::vector<FileEntry>& entries, int start,
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

void ensureSelectionVisible(BrowserState& state, const GridLayout& layout) {
  if (state.entries.empty() || layout.totalRows <= 0) {
    state.scrollRow = 0;
    return;
  }
  if (layout.totalRows <= layout.rowsVisible) {
    state.scrollRow = 0;
    return;
  }
  if (state.viewMode == BrowserState::ViewMode::ListOnly) {
    int visibleCapacity = browserGridVisibleCapacity(layout);
    int maxScroll = std::max(0, layout.totalRows - layout.rowsVisible);
    if (visibleCapacity <= 0 || maxScroll <= 0) {
      state.scrollRow = 0;
      return;
    }
    if (state.selected < state.scrollRow) {
      state.scrollRow = state.selected;
    } else if (state.selected >= state.scrollRow + visibleCapacity) {
      state.scrollRow = state.selected - visibleCapacity + 1;
    }
    state.scrollRow = std::clamp(state.scrollRow, 0, maxScroll);
    return;
  }

  int row = 0;
  int col = 0;
  getRowColFromIndex(state.selected, layout, state.viewMode, row, col);

  int maxScroll = std::max(0, layout.totalRows - layout.rowsVisible);
  if (row < state.scrollRow) {
    state.scrollRow = row;
  } else if (row >= state.scrollRow + layout.rowsVisible) {
    state.scrollRow = row - layout.rowsVisible + 1;
  }
  state.scrollRow = std::clamp(state.scrollRow, 0, maxScroll);
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
      ensureSelectionVisible(browser, layout);
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
    ensureSelectionVisible(browser, layout);
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
      ensureSelectionVisible(browser, layout);
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
    ensureSelectionVisible(browser, layout);
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

void handleInputEvent(const InputEvent& ev, BrowserState& browser,
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
  auto clearForwardHistory = [&]() { browser.forwardHistory.clear(); };
  auto pushBackHistory = [&](BrowserState::HistoryActionType type,
                             const std::filesystem::path& from,
                             const std::filesystem::path& to) {
    BrowserState::HistoryAction action;
    action.type = type;
    action.fromPath = from;
    action.toPath = to;
    browser.backHistory.push_back(action);
    clearForwardHistory();
  };
  auto navigateToDir = [&](const std::filesystem::path& dir) {
    browser.dir = dir;
    setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
    browser.selected = 0;
    if (callbacks.onRefreshBrowser) {
      callbacks.onRefreshBrowser(browser, "");
    }
    breadcrumbHover = -1;
    dirty = true;
  };
  auto undoBrowserBack = [&]() -> bool {
    if (decoderReady) {
      BrowserState::HistoryAction action;
      bool haveAction = false;
      if (!browser.backHistory.empty() &&
          browser.backHistory.back().type ==
              BrowserState::HistoryActionType::PlayFile) {
        action = browser.backHistory.back();
        browser.backHistory.pop_back();
        haveAction = true;
      } else if (callbacks.onCurrentPlaybackFile) {
        std::filesystem::path current = callbacks.onCurrentPlaybackFile();
        if (!current.empty()) {
          action.type = BrowserState::HistoryActionType::PlayFile;
          action.fromPath = browser.dir;
          action.toPath = current;
          haveAction = true;
        }
      }
      if (callbacks.onStopPlayback) {
        callbacks.onStopPlayback();
      }
      if (haveAction) {
        browser.forwardHistory.push_back(action);
      }
      dirty = true;
      return true;
    }

    while (!browser.backHistory.empty()) {
      BrowserState::HistoryAction action = browser.backHistory.back();
      browser.backHistory.pop_back();
      if (action.type == BrowserState::HistoryActionType::PlayFile) {
        browser.forwardHistory.push_back(action);
        continue;
      }
      if (action.type == BrowserState::HistoryActionType::EnterDirectory &&
          !action.fromPath.empty()) {
        navigateToDir(action.fromPath);
        browser.forwardHistory.push_back(action);
        return true;
      }
    }
    return false;
  };
  auto redoBrowserForward = [&]() -> bool {
    if (browser.forwardHistory.empty()) {
      return false;
    }
    BrowserState::HistoryAction action = browser.forwardHistory.back();
    browser.forwardHistory.pop_back();

    if (action.type == BrowserState::HistoryActionType::EnterDirectory) {
      if (!action.toPath.empty()) {
        navigateToDir(action.toPath);
      }
      browser.backHistory.push_back(action);
      return true;
    }

    if (action.type == BrowserState::HistoryActionType::PlayFile) {
      if (playMode && !action.toPath.empty() && callbacks.onPlayFile &&
          callbacks.onPlayFile(action.toPath)) {
        browser.backHistory.push_back(action);
        dirty = true;
        return true;
      }
      return false;
    }
    return false;
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
      target = browser.dir / target;
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

  auto applyPathSearch = [&]() {
    if (!browser.pathSearchActive) return;
    std::filesystem::path target;
    if (!resolvePathSearchTarget(browser.pathSearch, target)) return;
    if (target != browser.dir) {
      navigateToDir(target);
    } else {
      dirty = true;
    }
  };

  if (ev.type == InputEvent::Type::Resize) {
    dirty = true;
    if (callbacks.onResize) callbacks.onResize();
    return;
  }

  if (ev.type == InputEvent::Type::Key) {
    const KeyEvent& key = ev.key;
    bool browserBackKey = key.vk == VK_BROWSER_BACK;
    bool browserForwardKey = key.vk == VK_BROWSER_FORWARD;
    bool backspaceKey = key.vk == VK_BACK;
    const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
    bool ctrl = (key.control & ctrlMask) != 0;

    if (ctrl && (key.vk == 'Q' || key.ch == 'q' || key.ch == 'Q')) {
      running = false;
      if (callbacks.onQuit) callbacks.onQuit();
      dirty = true;
      return;
    }

    if (browserInteractionEnabled && browser.pathSearchActive) {
      if (key.vk == VK_ESCAPE || key.vk == VK_RETURN) {
        setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
        dirty = true;
        return;
      }
      if (browserBackKey) {
        undoBrowserBack();
        return;
      }
      if (browserForwardKey) {
        redoBrowserForward();
        return;
      }
      if (backspaceKey) {
        if (!browser.pathSearch.empty()) {
          browser.pathSearch.pop_back();
          if (browser.pathSearch.empty()) {
            dirty = true;
          } else {
            applyPathSearch();
          }
        }
        return;
      }
      if (key.ch >= 32) {
        browser.pathSearch.push_back(key.ch);
        applyPathSearch();
        return;
      }
      return;
    }

    if (browserInteractionEnabled && browser.filterActive) {
      if (key.vk == VK_ESCAPE) {
        browser.filter = browser.filterBackup;
        setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
        if (callbacks.onRefreshBrowser) callbacks.onRefreshBrowser(browser, "");
        return;
      }
      if (key.vk == VK_RETURN) {
        setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
        if (callbacks.onRefreshBrowser) callbacks.onRefreshBrowser(browser, "");
        return;
      }
      if (browserBackKey) {
        undoBrowserBack();
        return;
      }
      if (browserForwardKey) {
        redoBrowserForward();
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

    if ((playMode || decoderReady) && handlePlaybackInput(ev, callbacks)) {
      if (key.vk == 'O' || key.ch == 'o' || key.ch == 'O') {
        clearForwardHistory();
      }
      dirty = true;
      return;
    }
    if (browserBackKey) {
      undoBrowserBack();
      return;
    }
    if (browserForwardKey) {
      redoBrowserForward();
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
      if (callbacks.onRefreshBrowser) callbacks.onRefreshBrowser(browser, "");
      dirty = true;
      return;
    }
    if (key.vk == VK_ESCAPE) {
      if (callbacks.onStopPlayback) {
        clearForwardHistory();
        callbacks.onStopPlayback();
        dirty = true;
      }
      return;
    }
    if (backspaceKey) {
      clearForwardHistory();
      if (optionsBrowserIsActive(browser)) {
        if (optionsBrowserNavigateUp(browser)) {
          if (callbacks.onRefreshBrowser) {
            callbacks.onRefreshBrowser(browser, "");
          }
          breadcrumbHover = -1;
          dirty = true;
        }
        return;
      }
      if (browser.dir.has_parent_path()) {
        navigateToDir(browser.dir.parent_path());
      }
      return;
    }
    if (key.vk == VK_RETURN) {
      int count = static_cast<int>(browser.entries.size());
      if (count > 0) {
        const auto& pick = browser.entries[static_cast<size_t>(browser.selected)];
        if (pick.isSectionHeader) return;
        if (ctrl && playMode && !pick.isDir) {
          if (callbacks.onOpenFileContextMenu) {
            callbacks.onOpenFileContextMenu(pick, -1, -1);
            dirty = true;
          }
          return;
        }
        if (pick.isDir) {
          pushBackHistory(BrowserState::HistoryActionType::EnterDirectory,
                          browser.dir, pick.path);
          navigateToDir(pick.path);
        } else if (playMode) {
          if (callbacks.onPlayFile && callbacks.onPlayFile(pick.path)) {
            pushBackHistory(BrowserState::HistoryActionType::PlayFile, browser.dir,
                            pick.path);
            dirty = true;
          }
        } else {
          clearForwardHistory();
          if (callbacks.onRenderFile) {
            callbacks.onRenderFile(pick.path);
          }
          running = false;
        }
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
      if (browser.dir != crumb.path) {
        pushBackHistory(BrowserState::HistoryActionType::EnterDirectory, browser.dir,
                        crumb.path);
        navigateToDir(crumb.path);
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
            clearForwardHistory();
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
      ProgressBarHitTestInput progressHit;
      progressHit.x = mouse.hasPixelPosition ? mouse.pixelX : mouse.pos.X;
      progressHit.y = mouse.hasPixelPosition ? mouse.pixelY : mouse.pos.Y;
      progressHit.barX = progressBarX;
      progressHit.barY = progressBarY;
      progressHit.barWidth = progressBarWidth;
      progressHit.unitWidth = mouse.hasPixelPosition ? mouse.unitWidth : 1.0;
      progressHit.unitHeight = mouse.hasPixelPosition ? mouse.unitHeight : 1.0;
      double ratio = 0.0;
      if (progressBarRatioAt(progressHit, &ratio)) {
        if (callbacks.onSeekToRatio) callbacks.onSeekToRatio(ratio);
        dirty = true;
        return;
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
      int hoverIdx = nearestSelectableEntryAny(browser.entries, idx);
      if (hoverIdx >= 0 && browser.selected != hoverIdx) {
        browser.selected = hoverIdx;
        dirty = true;
      }
      return;
    }

    if (mouse.eventFlags == 0 && rightPressed) {
      if (browser.selected != idx) {
        int hoverIdx = nearestSelectableEntryAny(browser.entries, idx);
        if (hoverIdx < 0) return;
        if (browser.selected != hoverIdx) {
          browser.selected = hoverIdx;
          dirty = true;
        }
      }
      if (!isSelectableEntry(browser.entries[static_cast<size_t>(browser.selected)])) {
        return;
      }
      const auto& pick = browser.entries[static_cast<size_t>(browser.selected)];
      if (!pick.isDir && callbacks.onOpenFileContextMenu) {
        callbacks.onOpenFileContextMenu(pick, mouse.pos.X, mouse.pos.Y);
        dirty = true;
      }
      return;
    }

    if (mouse.eventFlags == 0 && leftPressed) {
      int clickIdx = nearestSelectableEntryAny(browser.entries, idx);
      if (clickIdx < 0) return;
      if (browser.selected != clickIdx) {
        browser.selected = clickIdx;
        dirty = true;
      }
      const auto& pick = browser.entries[static_cast<size_t>(browser.selected)];
      if (pick.isDir) {
        pushBackHistory(BrowserState::HistoryActionType::EnterDirectory, browser.dir,
                        pick.path);
        navigateToDir(pick.path);
      } else if (playMode) {
        if (callbacks.onPlayFile && callbacks.onPlayFile(pick.path)) {
          pushBackHistory(BrowserState::HistoryActionType::PlayFile, browser.dir,
                          pick.path);
          dirty = true;
        }
      } else {
        clearForwardHistory();
        if (callbacks.onRenderFile) {
          callbacks.onRenderFile(pick.path);
        }
        running = false;
      }
    }
  }
}

bool handlePlaybackInput(const InputEvent& ev,
                         const InputCallbacks& callbacks) {
  if (ev.type == InputEvent::Type::Key) {
    const KeyEvent& key = ev.key;
    const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
    const DWORD shiftMask = SHIFT_PRESSED;
    bool ctrl = (key.control & ctrlMask) != 0;
    bool shift = (key.control & shiftMask) != 0;

    if (ctrl && (key.vk == 'Q' || key.ch == 'q' || key.ch == 'Q')) {
      if (callbacks.onQuit) callbacks.onQuit();
      return true;
    }
    if (key.vk == kPlaybackVkMediaPlay) {
      if (callbacks.onPlay) callbacks.onPlay();
      return true;
    }
    if (key.vk == kPlaybackVkMediaPause) {
      if (callbacks.onPause) callbacks.onPause();
      return true;
    }
    if (key.vk == VK_SPACE || key.ch == ' ') {
      if (callbacks.onTogglePause) callbacks.onTogglePause();
      return true;
    }
    if (key.vk == VK_MEDIA_PLAY_PAUSE) {
      if (callbacks.onTogglePause) callbacks.onTogglePause();
      return true;
    }
    if (key.vk == VK_MEDIA_STOP) {
      if (callbacks.onStopPlayback) callbacks.onStopPlayback();
      return true;
    }
    if (key.vk == VK_MEDIA_PREV_TRACK) {
      if (callbacks.onPlayPrevious) callbacks.onPlayPrevious();
      return true;
    }
    if (key.vk == VK_MEDIA_NEXT_TRACK) {
      if (callbacks.onPlayNext) callbacks.onPlayNext();
      return true;
    }
    if (key.vk == 'W' || key.ch == 'w' || key.ch == 'W') {
      if (callbacks.onToggleWindow) callbacks.onToggleWindow();
      return true;
    }
    if (key.vk == 'R' || key.ch == 'r' || key.ch == 'R') {
      if (callbacks.onToggleRadio) callbacks.onToggleRadio();
      return true;
    }
    if (key.vk == 'H' || key.ch == 'h' || key.ch == 'H') {
      if (callbacks.onToggle50Hz) callbacks.onToggle50Hz();
      return true;
    }
    if (key.vk == 'S' || key.ch == 's' || key.ch == 'S') {
      if (callbacks.onToggleSubtitles) callbacks.onToggleSubtitles();
      return true;
    }
    if (key.vk == 'A' || key.ch == 'a' || key.ch == 'A') {
      if (callbacks.onToggleAudioTrack) callbacks.onToggleAudioTrack();
      return true;
    }
    if (key.vk == 'O' || key.ch == 'o' || key.ch == 'O') {
      if (callbacks.onToggleOptions) callbacks.onToggleOptions();
      return true;
    }

    if (key.vk == VK_OEM_4 || key.ch == '[') {
      if (callbacks.onSeekBy) callbacks.onSeekBy(-1);
      return true;
    }
    if (key.vk == VK_OEM_6 || key.ch == ']') {
      if (callbacks.onSeekBy) callbacks.onSeekBy(1);
      return true;
    }
    if (ctrl && (key.vk == VK_LEFT || key.vk == VK_RIGHT)) {
      if (callbacks.onSeekBy) callbacks.onSeekBy((key.vk == VK_LEFT) ? -1 : 1);
      return true;
    }

    if (key.vk == VK_UP) {
      if (shift) {
        float step = 0.10f;
        if (callbacks.onAdjustVolume) callbacks.onAdjustVolume(step);
        return true;
      }
    }
    if (key.vk == VK_DOWN) {
      if (shift) {
        float step = 0.10f;
        if (callbacks.onAdjustVolume) callbacks.onAdjustVolume(-step);
        return true;
      }
    }
  }
  return false;
}
