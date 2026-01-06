#include "consoleinput.h"

#include <algorithm>

#include "consolescreen.h"

void ConsoleInput::init() {
  handle_ = GetStdHandle(STD_INPUT_HANDLE);
  if (handle_ == INVALID_HANDLE_VALUE) return;
  if (!GetConsoleMode(handle_, &originalMode_)) return;
  DWORD mode = originalMode_;
  mode |= ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
  mode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
  mode |= ENABLE_EXTENDED_FLAGS;
  if (!SetConsoleMode(handle_, mode)) return;
  active_ = true;
}

void ConsoleInput::restore() {
  if (active_) {
    SetConsoleMode(handle_, originalMode_);
  }
}

bool ConsoleInput::poll(InputEvent& out) {
  if (!active_) return false;
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
void ensureSelectionVisible(BrowserState& state, const GridLayout& layout) {
  if (state.entries.empty()) {
    state.scrollRow = 0;
    return;
  }
  if (layout.totalRows <= layout.rowsVisible) {
    state.scrollRow = 0;
    return;
  }
  int cols = std::max(1, layout.cols);
  int row = state.selected / cols;
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

  if (deltaRow == 0 && deltaCol != 0) {
    int next = browser.selected + deltaCol;
    if (next < 0 || next >= count) return;
    if (next != browser.selected) {
      browser.selected = next;
      ensureSelectionVisible(browser, layout);
      dirty = true;
    }
    return;
  }

  int cols = std::max(1, layout.cols);
  int row = browser.selected / cols;
  int col = browser.selected % cols;
  int nextRow = row + deltaRow;
  if (nextRow < 0 || nextRow >= layout.totalRows) return;
  int rowStart = nextRow * cols;
  int rowCount = std::min(cols, count - rowStart);
  if (rowCount <= 0) return;
  int nextCol = std::clamp(col + deltaCol, 0, rowCount - 1);
  int idx = rowStart + nextCol;
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
  int cols = std::max(1, layout.cols);
  int row = browser.selected / cols;
  int col = browser.selected % cols;
  int step = std::max(1, layout.rowsVisible);
  int nextRow = row + direction * step;
  nextRow = std::clamp(nextRow, 0, layout.totalRows - 1);
  int rowStart = nextRow * cols;
  int rowCount = std::min(cols, count - rowStart);
  if (rowCount <= 0) return;
  int nextCol = std::min(col, rowCount - 1);
  int idx = rowStart + nextCol;
  if (idx != browser.selected) {
    browser.selected = idx;
    ensureSelectionVisible(browser, layout);
    dirty = true;
  }
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
}  // namespace

void handleInputEvent(const InputEvent& ev, BrowserState& browser,
                      const GridLayout& layout,
                      const BreadcrumbLine& breadcrumbLine, int breadcrumbY,
                      int listTop, int listHeight, int progressBarX,
                      int progressBarY, int progressBarWidth, bool playMode,
                      bool decoderReady, int& breadcrumbHover, bool& dirty,
                      bool& running, const InputCallbacks& callbacks) {
  if (ev.type == InputEvent::Type::Resize) {
    dirty = true;
    if (callbacks.onResize) callbacks.onResize();
    return;
  }

  if (ev.type == InputEvent::Type::Key) {
    const KeyEvent& key = ev.key;
    const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
    bool ctrl = (key.control & ctrlMask) != 0;
    if ((key.vk == 'C' || key.ch == 'c' || key.ch == 'C') && ctrl) {
      running = false;
      if (callbacks.onQuit) callbacks.onQuit();
      return;
    }
    if (key.vk == 'Q' || key.ch == 'q' || key.ch == 'Q') {
      running = false;
      if (callbacks.onQuit) callbacks.onQuit();
      return;
    }
    if (key.vk == VK_BACK) {
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
        if (pick.isDir) {
          browser.dir = pick.path;
          browser.selected = 0;
          if (callbacks.onRefreshBrowser) {
            callbacks.onRefreshBrowser(browser, "");
          }
          breadcrumbHover = -1;
          dirty = true;
        } else if (playMode) {
          if (callbacks.onPlayFile && callbacks.onPlayFile(pick.path)) {
            dirty = true;
          }
        } else {
          if (callbacks.onRenderFile) {
            callbacks.onRenderFile(pick.path);
          }
          running = false;
        }
      }
      return;
    }
    if (key.vk == VK_SPACE || key.ch == ' ') {
      if (playMode && decoderReady) {
        if (callbacks.onTogglePause) callbacks.onTogglePause();
        dirty = true;
      }
      return;
    }
    if (key.vk == 'R' || key.ch == 'r' || key.ch == 'R') {
      if (callbacks.onToggleRadio) callbacks.onToggleRadio();
      dirty = true;
      return;
    }
    if (key.vk == 'T' || key.ch == 't' || key.ch == 'T') {
      browser.thumbsEnabled = !browser.thumbsEnabled;
      dirty = true;
      return;
    }
    if (key.vk == VK_LEFT) {
      if (ctrl) {
        if (callbacks.onSeekBy) callbacks.onSeekBy(-1);
        dirty = true;
      } else {
        moveSelection(browser, layout, -1, 0, dirty);
      }
      return;
    }
    if (key.vk == VK_RIGHT) {
      if (ctrl) {
        if (callbacks.onSeekBy) callbacks.onSeekBy(1);
        dirty = true;
      } else {
        moveSelection(browser, layout, 1, 0, dirty);
      }
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
    int nextHover = breadcrumbIndexAt(breadcrumbLine, mouse.pos.X, mouse.pos.Y,
                                      breadcrumbY);
    if (nextHover != breadcrumbHover) {
      breadcrumbHover = nextHover;
      dirty = true;
    }
    if (mouse.eventFlags == MOUSE_WHEELED) {
      int delta = static_cast<SHORT>(HIWORD(mouse.buttonState));
      if (delta != 0) {
        browser.scrollRow -= delta / WHEEL_DELTA;
        int maxScroll =
            std::max(0, layout.totalRows - layout.rowsVisible);
        browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
        dirty = true;
      }
      return;
    }

    if (leftPressed && mouse.eventFlags == 0 && breadcrumbHover >= 0) {
      const auto& crumb =
          breadcrumbLine.crumbs[static_cast<size_t>(breadcrumbHover)];
      if (browser.dir != crumb.path) {
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

    if (leftPressed &&
        (mouse.eventFlags == 0 || mouse.eventFlags == MOUSE_MOVED)) {
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
    int idx = row * layout.cols + col;
    if (idx < 0 || idx >= count) return;

    if (mouse.eventFlags == MOUSE_MOVED && !leftPressed) {
      if (browser.selected != idx) {
        browser.selected = idx;
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
        browser.dir = pick.path;
        browser.selected = 0;
        if (callbacks.onRefreshBrowser) {
          callbacks.onRefreshBrowser(browser, "");
        }
        breadcrumbHover = -1;
        dirty = true;
      } else if (playMode) {
        if (callbacks.onPlayFile && callbacks.onPlayFile(pick.path)) {
          dirty = true;
        }
      } else {
        if (callbacks.onRenderFile) {
          callbacks.onRenderFile(pick.path);
        }
        running = false;
      }
    }
  }
}
