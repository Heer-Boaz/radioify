#include "consoleinput.h"

#include <algorithm>

#include "consolescreen.h"
#include "optionsbrowser.h"

bool ConsoleInput::hasInputFocus() const {
  // Trust console focus events for pseudoconsole hosts (Windows Terminal,
  // ConPTY, etc.) where foreground-window ownership checks are unreliable.
  return focusActive_;
}

void ConsoleInput::init() {
  handle_ = GetStdHandle(STD_INPUT_HANDLE);
  if (handle_ == INVALID_HANDLE_VALUE) return;
  if (!GetConsoleMode(handle_, &originalMode_)) return;
  DWORD mode = originalMode_;
  mode |= ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
  mode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
  mode |= ENABLE_EXTENDED_FLAGS;
  if (!SetConsoleMode(handle_, mode)) return;
  focusActive_ = true;
  active_ = true;
}

void ConsoleInput::restore() {
  if (active_) {
    SetConsoleMode(handle_, originalMode_);
  }
}

bool ConsoleInput::poll(InputEvent& out) {
  if (!active_) return false;

  // Fallback for terminal hosts that don't forward XBUTTON mouse events
  // through ReadConsoleInput. Detect side-button edges via async key state.
  bool focused = hasInputFocus();
  bool x1Down = (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
  bool x2Down = (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
  bool x1Pressed = focused && x1Down && !xButton1Prev_;
  bool x2Pressed = focused && x2Down && !xButton2Prev_;
  xButton1Prev_ = x1Down;
  xButton2Prev_ = x2Down;
  if (x1Pressed) {
    out.type = InputEvent::Type::Key;
    out.key.vk = VK_BROWSER_BACK;
    out.key.ch = 0;
    out.key.control = 0;
    return true;
  }
  if (x2Pressed) {
    out.type = InputEvent::Type::Key;
    out.key.vk = VK_BROWSER_FORWARD;
    out.key.ch = 0;
    out.key.control = 0;
    return true;
  }

  DWORD count = 0;
  if (!GetNumberOfConsoleInputEvents(handle_, &count) || count == 0)
    return false;
  while (count > 0) {
    INPUT_RECORD rec{};
    DWORD read = 0;
    if (!ReadConsoleInput(handle_, &rec, 1, &read) || read == 0) return false;
    if (rec.EventType == KEY_EVENT) {
      const auto& kev = rec.Event.KeyEvent;
      if (!kev.bKeyDown) {
        count--;
        continue;
      }
      out.type = InputEvent::Type::Key;
      out.key.vk = kev.wVirtualKeyCode;
      out.key.ch = static_cast<char>(kev.uChar.AsciiChar);
      out.key.control = kev.dwControlKeyState;
      return true;
    }
    if (rec.EventType == MOUSE_EVENT) {
      const auto& mev = rec.Event.MouseEvent;
      if (mev.dwEventFlags == 0) {
        DWORD sideMask = FROM_LEFT_2ND_BUTTON_PRESSED |
                         FROM_LEFT_3RD_BUTTON_PRESSED |
                         FROM_LEFT_4TH_BUTTON_PRESSED;
        if ((mev.dwButtonState & sideMask) != 0) {
          out.type = InputEvent::Type::Key;
          if ((mev.dwButtonState & FROM_LEFT_2ND_BUTTON_PRESSED) != 0) {
            out.key.vk = VK_BROWSER_BACK;
          } else {
            out.key.vk = VK_BROWSER_FORWARD;
          }
          out.key.ch = 0;
          out.key.control = mev.dwControlKeyState;
          return true;
        }
      }
      out.type = InputEvent::Type::Mouse;
      out.mouse.pos = mev.dwMousePosition;
      out.mouse.buttonState = mev.dwButtonState;
      out.mouse.eventFlags = mev.dwEventFlags;
      out.mouse.control = mev.dwControlKeyState;
      return true;
    }
    if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
      out.type = InputEvent::Type::Resize;
      out.size = rec.Event.WindowBufferSizeEvent.dwSize;
      return true;
    }
    if (rec.EventType == FOCUS_EVENT) {
      focusActive_ = rec.Event.FocusEvent.bSetFocus != FALSE;
      count--;
      continue;
    }
    count--;
  }
  return false;
}

bool ConsoleInput::active() const { return active_; }

std::vector<DriveEntry> listDriveEntries() {
  std::vector<DriveEntry> drives;
#ifdef _WIN32
  DWORD mask = GetLogicalDrives();
  if (mask == 0) return drives;
  for (int i = 0; i < 26; ++i) {
    if ((mask & (1u << i)) == 0) continue;
    char letter = static_cast<char>('A' + i);
    std::string label;
    label.push_back(letter);
    label.push_back(':');
    std::string root;
    root.push_back(letter);
    root.append(":\\");
    drives.push_back(DriveEntry{label, std::filesystem::path(root)});
  }
#endif
  return drives;
}

namespace {
int getEntryIndex(int row, int col, int totalRows, int cols, int count,
                  BrowserState::ViewMode mode) {
  if (mode == BrowserState::ViewMode::ListOnly) {
    int idx = col * totalRows + row;
    return (idx >= 0 && idx < count) ? idx : -1;
  } else {
    int idx = row * cols + col;
    return (idx >= 0 && idx < count) ? idx : -1;
  }
}

void getRowColFromIndex(int idx, int totalRows, int cols,
                        BrowserState::ViewMode mode, int& row, int& col) {
  if (mode == BrowserState::ViewMode::ListOnly) {
    if (totalRows <= 0) {
      row = 0;
      col = 0;
    } else {
      col = idx / totalRows;
      row = idx % totalRows;
    }
  } else {
    if (cols <= 0) {
      row = 0;
      col = 0;
    } else {
      row = idx / cols;
      col = idx % cols;
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
  int row = 0;
  int col = 0;
  getRowColFromIndex(state.selected, layout.totalRows, layout.cols,
                     state.viewMode, row, col);

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

  int row = 0;
  int col = 0;
  getRowColFromIndex(browser.selected, layout.totalRows, layout.cols,
                     browser.viewMode, row, col);

  int nextRow = std::clamp(row + deltaRow, 0, layout.totalRows - 1);
  int nextCol = std::clamp(col + deltaCol, 0, layout.cols - 1);

  int idx = getEntryIndex(nextRow, nextCol, layout.totalRows, layout.cols, count,
                          browser.viewMode);
  if (idx < 0) {
    if (deltaCol > 0 || deltaRow > 0)
      idx = count - 1;
    else
      idx = 0;
  }

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

  int row = 0;
  int col = 0;
  getRowColFromIndex(browser.selected, layout.totalRows, layout.cols,
                     browser.viewMode, row, col);

  int step = std::max(1, layout.rowsVisible);
  int nextRow = std::clamp(row + direction * step, 0, layout.totalRows - 1);

  int idx = getEntryIndex(nextRow, col, layout.totalRows, layout.cols, count,
                          browser.viewMode);
  if (idx < 0) idx = count - 1;

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
  if (layout.y < 0 || y != layout.y) return -1;
  int count = static_cast<int>(layout.buttons.size());
  for (int i = 0; i < count; ++i) {
    const auto& btn = layout.buttons[static_cast<size_t>(i)];
    if (x >= btn.x0 && x < btn.x1) return i;
  }
  return -1;
}
}  // namespace

void handleInputEvent(const InputEvent& ev, BrowserState& browser,
                      const GridLayout& layout,
                      const BreadcrumbLine& breadcrumbLine, int breadcrumbY,
                      int listTop, int listHeight, int progressBarX,
                      int progressBarY, int progressBarWidth,
                      const ActionStripLayout& actionStrip,
                      bool browserInteractionEnabled, bool playMode,
                      bool decoderReady, int& breadcrumbHover, int& actionHover,
                      bool& dirty, bool& running,
                      const InputCallbacks& callbacks) {
  auto clearForwardHistory = [&]() {
    browser.forwardHistory.clear();
  };
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

  if (ev.type == InputEvent::Type::Resize) {
    dirty = true;
    if (callbacks.onResize) callbacks.onResize();
    return;
  }

  if (ev.type == InputEvent::Type::Key) {
    const KeyEvent& key = ev.key;
    bool browserBackKey =
        (key.vk == VK_BROWSER_BACK) || (key.vk == VK_BACK && key.ch == 0);
    bool browserForwardKey = key.vk == VK_BROWSER_FORWARD;
    bool keyboardBackspace = key.vk == VK_BACK && key.ch != 0;
    const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
    const DWORD altMask = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
    const DWORD shiftMask = SHIFT_PRESSED;
    bool ctrl = (key.control & ctrlMask) != 0;
    bool alt = (key.control & altMask) != 0;
    bool shift = (key.control & shiftMask) != 0;

    if (ctrl && (key.vk == 'Q' || key.ch == 'q' || key.ch == 'Q')) {
      running = false;
      if (callbacks.onQuit) callbacks.onQuit();
      dirty = true;
      return;
    }

    if (browserInteractionEnabled && browser.filterActive) {
      if (key.vk == VK_ESCAPE || key.vk == VK_RETURN) {
        browser.filterActive = false;
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
      if (keyboardBackspace) {
        if (!browser.filter.empty()) {
          browser.filter.pop_back();
          if (callbacks.onRefreshBrowser)
            callbacks.onRefreshBrowser(browser, "");
          dirty = true;
        }
        return;
      }
      if (key.ch >= 32) {
        browser.filter += key.ch;
        if (callbacks.onRefreshBrowser)
            callbacks.onRefreshBrowser(browser, "");
        dirty = true;
        return;
      }
      return;
    }

    if ((playMode || decoderReady) &&
        handlePlaybackInput(ev, running, callbacks)) {
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
    if (key.vk == VK_DIVIDE || key.ch == '/') {
      browser.filterActive = true;
      browser.filter.clear();
      dirty = true;
      return;
    }
    if (key.vk == 'S' || key.ch == 's' || key.ch == 'S') {
      const DWORD altMask = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
      bool alt = (key.control & altMask) != 0;
      if (alt) {
        browser.sortDescending = !browser.sortDescending;
      } else {
        int next = static_cast<int>(browser.sortMode) + 1;
        if (next > static_cast<int>(BrowserState::SortMode::Size)) next = 0;
        browser.sortMode = static_cast<BrowserState::SortMode>(next);

        // Set logical defaults for new mode
        if (browser.sortMode == BrowserState::SortMode::Name)
          browser.sortDescending = false;
        else
          browser.sortDescending = true;
      }
      if (callbacks.onRefreshBrowser)
        callbacks.onRefreshBrowser(browser, "");
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
    if (keyboardBackspace) {
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
        browser.dir = browser.dir.parent_path();
        browser.selected = 0;
        if (callbacks.onRefreshBrowser) {
          callbacks.onRefreshBrowser(browser, "");
        }
        breadcrumbHover = -1;
        dirty = true;
      }
      return;
    }
    if (key.vk == VK_RETURN) {
      int count = static_cast<int>(browser.entries.size());
      if (count > 0) {
        const auto& pick =
            browser.entries[static_cast<size_t>(browser.selected)];
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
          browser.dir = pick.path;
          browser.selected = 0;
          if (callbacks.onRefreshBrowser) {
            callbacks.onRefreshBrowser(browser, "");
          }
          breadcrumbHover = -1;
          dirty = true;
        } else if (playMode) {
          if (callbacks.onPlayFile && callbacks.onPlayFile(pick.path)) {
            pushBackHistory(BrowserState::HistoryActionType::PlayFile,
                            browser.dir, pick.path);
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
    if (browserInteractionEnabled) {
      int nextHover = breadcrumbIndexAt(breadcrumbLine, mouse.pos.X, mouse.pos.Y,
                                        breadcrumbY);
      if (nextHover != breadcrumbHover) {
        breadcrumbHover = nextHover;
        dirty = true;
      }
    } else if (breadcrumbHover != -1) {
      breadcrumbHover = -1;
      dirty = true;
    }
    int nextActionHover =
        actionStripIndexAt(actionStrip, mouse.pos.X, mouse.pos.Y);
    if (nextActionHover != actionHover) {
      actionHover = nextActionHover;
      dirty = true;
    }
    if (mouse.eventFlags == MOUSE_WHEELED) {
      int delta = static_cast<SHORT>(HIWORD(mouse.buttonState));
      if (delta != 0) {
        int actionIndex =
            actionStripIndexAt(actionStrip, mouse.pos.X, mouse.pos.Y);
        if (actionIndex >= 0) {
          const auto& btn =
              actionStrip.buttons[static_cast<size_t>(actionIndex)];
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
        int maxScroll =
            std::max(0, layout.totalRows - layout.rowsVisible);
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
        pushBackHistory(BrowserState::HistoryActionType::EnterDirectory,
                        browser.dir, crumb.path);
        browser.dir = crumb.path;
        browser.selected = 0;
        if (callbacks.onRefreshBrowser) {
          callbacks.onRefreshBrowser(browser, "");
        }
        breadcrumbHover = -1;
        dirty = true;
      }
      return;
    }

    if (browserInteractionEnabled && rightPressed && mouse.eventFlags == 0) {
      int actionIndex =
          actionStripIndexAt(actionStrip, mouse.pos.X, mouse.pos.Y);
      if (actionIndex >= 0) {
        const auto& btn =
            actionStrip.buttons[static_cast<size_t>(actionIndex)];
        if (btn.id == ActionStripItem::View) {
          browser.viewMode = prevViewMode(browser.viewMode);
          dirty = true;
          return;
        }
      }
    }

    if (leftPressed &&
        (mouse.eventFlags == 0 || mouse.eventFlags == MOUSE_MOVED)) {
      int actionIndex =
          actionStripIndexAt(actionStrip, mouse.pos.X, mouse.pos.Y);
      if (actionIndex >= 0) {
        const auto& btn =
            actionStrip.buttons[static_cast<size_t>(actionIndex)];
        switch (btn.id) {
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
          case ActionStripItem::MelodyViz:
            if (callbacks.onToggleMelodyVisualization) {
              callbacks.onToggleMelodyVisualization();
            }
            dirty = true;
            return;
        }
      }
      if (layout.showScrollBar && layout.scrollBarX >= 0 &&
          layout.scrollBarWidth > 0 &&
          mouse.pos.X >= layout.scrollBarX &&
          mouse.pos.X < layout.scrollBarX + layout.scrollBarWidth &&
          mouse.pos.Y >= listTop && mouse.pos.Y < listTop + listHeight) {
        scrollFromBar(browser, layout, mouse.pos.Y, listTop, listHeight,
                      dirty);
        return;
      }
      if (progressBarWidth > 0 && mouse.pos.Y == progressBarY &&
          progressBarX >= 0) {
        int rel = mouse.pos.X - progressBarX;
        if (rel >= 0 && rel < progressBarWidth) {
          double denom = static_cast<double>(std::max(1, progressBarWidth - 1));
          double ratio = static_cast<double>(rel) / denom;
          if (callbacks.onSeekToRatio) callbacks.onSeekToRatio(ratio);
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
    int idx = getEntryIndex(row, col, layout.totalRows, layout.cols, count,
                            browser.viewMode);
    if (idx < 0 || idx >= count) return;

    if (mouse.eventFlags == MOUSE_MOVED && !leftPressed) {
      if (browser.selected != idx) {
        browser.selected = idx;
        dirty = true;
      }
      return;
    }

    if (mouse.eventFlags == 0 && rightPressed) {
      if (browser.selected != idx) {
        browser.selected = idx;
        dirty = true;
      }
      const auto& pick = browser.entries[static_cast<size_t>(browser.selected)];
      if (!pick.isDir && callbacks.onOpenFileContextMenu) {
        callbacks.onOpenFileContextMenu(pick, mouse.pos.X, mouse.pos.Y);
        dirty = true;
      }
      return;
    }

    if (mouse.eventFlags == 0 && leftPressed) {
      if (browser.selected != idx) {
        browser.selected = idx;
        dirty = true;
      }
      const auto& pick = browser.entries[static_cast<size_t>(browser.selected)];
      if (pick.isDir) {
        pushBackHistory(BrowserState::HistoryActionType::EnterDirectory,
                        browser.dir, pick.path);
        browser.dir = pick.path;
        browser.selected = 0;
        if (callbacks.onRefreshBrowser) {
          callbacks.onRefreshBrowser(browser, "");
        }
        breadcrumbHover = -1;
        dirty = true;
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

bool handlePlaybackInput(const InputEvent& ev, bool& running,
                          const InputCallbacks& callbacks) {
  if (ev.type == InputEvent::Type::Key) {
    const KeyEvent& key = ev.key;
    const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
    const DWORD altMask = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
    const DWORD shiftMask = SHIFT_PRESSED;
    bool ctrl = (key.control & ctrlMask) != 0;
    bool alt = (key.control & altMask) != 0;
    bool shift = (key.control & shiftMask) != 0;

    // Exit shortcut
    if (ctrl && (key.vk == 'Q' || key.ch == 'q' || key.ch == 'Q')) {
      running = false;
      if (callbacks.onQuit) callbacks.onQuit();
      return true;
    }
    // Playback control
    if (key.vk == VK_SPACE || key.ch == ' ') {
      if (callbacks.onTogglePause) callbacks.onTogglePause();
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
    if (key.vk == 'O' || key.ch == 'o' || key.ch == 'O') {
      if (callbacks.onToggleOptions) callbacks.onToggleOptions();
      return true;
    }
    if (key.vk == 'V' || key.ch == 'v' || key.ch == 'V') {
      if (callbacks.onToggleVsync) callbacks.onToggleVsync();
      return true;
    }
    if (key.vk == 'M' || key.ch == 'm' || key.ch == 'M') {
      if (callbacks.onToggleMelodyVisualization) callbacks.onToggleMelodyVisualization();
      return true;
    }

    // Seek
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

    // Radio makeup gain
    if (ctrl && key.vk == VK_UP) {
      if (callbacks.onAdjustRadioMakeup) callbacks.onAdjustRadioMakeup(0.05f);
      return true;
    }
    if (ctrl && key.vk == VK_DOWN) {
      if (callbacks.onAdjustRadioMakeup) callbacks.onAdjustRadioMakeup(-0.05f);
      return true;
    }

    // Volume
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
