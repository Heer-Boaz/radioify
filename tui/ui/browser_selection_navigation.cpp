#include "browser_selection_navigation.h"

#include <algorithm>

#include "browser_grid_index.h"
#include "browser_navigation.h"

namespace browser_selection_navigation {
namespace {

int nearestSelectableEntry(const std::vector<BrowserEntry>& entries, int start,
                           int direction) {
  if (entries.empty()) return 0;
  const int count = static_cast<int>(entries.size());
  int index = std::clamp(start, 0, count - 1);
  const int step = direction >= 0 ? 1 : -1;
  for (int visited = 0; visited < count; ++visited) {
    if (entries[static_cast<std::size_t>(index)].isSelectable()) return index;
    index += step;
    if (index >= count) index = 0;
    if (index < 0) index = count - 1;
  }
  return start;
}

void rowAndColumn(int index, const GridLayout& layout,
                  BrowserState::ViewMode mode, int& row, int& column) {
  if (mode == BrowserState::ViewMode::ListOnly) {
    const int stride = std::max(1, layout.rowsVisible);
    column = index / stride;
    row = index % stride;
    return;
  }
  const int columns = std::max(1, layout.cols);
  row = index / columns;
  column = index % columns;
}

bool moveSelection(BrowserState& browser, const GridLayout& layout,
                   int deltaColumn, int deltaRow) {
  const int count = static_cast<int>(browser.entries.size());
  if (count == 0 || layout.totalRows <= 0 || layout.cols <= 0) return false;

  int index = browser.selected;
  if (browser.viewMode == BrowserState::ViewMode::ListOnly) {
    index += deltaColumn != 0
                 ? deltaColumn * std::max(1, layout.rowsVisible)
                 : deltaRow;
    index = std::clamp(index, 0, count - 1);
  } else {
    int row = 0;
    int column = 0;
    rowAndColumn(browser.selected, layout, browser.viewMode, row, column);
    const int nextRow =
        std::clamp(row + deltaRow, 0, layout.totalRows - 1);
    const int nextColumn =
        std::clamp(column + deltaColumn, 0, layout.cols - 1);
    index = browserGridEntryIndex(layout, browser.viewMode, nextRow,
                                  nextColumn, count);
    if (index < 0) {
      index = deltaColumn > 0 || deltaRow > 0 ? count - 1 : 0;
    }
  }

  const int direction = deltaColumn > 0 || deltaRow > 0 ? 1 : -1;
  index = nearestSelectableEntry(browser.entries, index, direction);
  if (index == browser.selected) return false;
  browser.selected = index;
  ensureBrowserSelectionVisible(browser, layout);
  return true;
}

bool pageSelection(BrowserState& browser, const GridLayout& layout,
                   int direction) {
  const int count = static_cast<int>(browser.entries.size());
  if (count == 0 || layout.totalRows <= 0 || layout.cols <= 0) return false;

  const int step = std::max(1, layout.rowsVisible);
  int index = browser.selected;
  if (browser.viewMode == BrowserState::ViewMode::ListOnly) {
    index = std::clamp(index + direction * step, 0, count - 1);
  } else {
    int row = 0;
    int column = 0;
    rowAndColumn(browser.selected, layout, browser.viewMode, row, column);
    const int nextRow =
        std::clamp(row + direction * step, 0, layout.totalRows - 1);
    index = browserGridEntryIndex(layout, browser.viewMode, nextRow, column,
                                  count);
    if (index < 0) index = count - 1;
  }
  index = nearestSelectableEntry(browser.entries, index, direction);
  if (index == browser.selected) return false;
  browser.selected = index;
  ensureBrowserSelectionVisible(browser, layout);
  return true;
}

}  // namespace

bool apply(BrowserState& browser, const GridLayout& layout,
           browser_input::KeyAction action, std::uint32_t repetitions) {
  const std::uint32_t normalized = repetitions == 0 ? 1 : repetitions;
  const std::uint32_t usefulCount = static_cast<std::uint32_t>(
      std::min<std::size_t>(normalized,
                            std::max<std::size_t>(1, browser.entries.size())));
  bool changed = false;
  for (std::uint32_t repeat = 0; repeat < usefulCount; ++repeat) {
    bool moved = false;
    switch (action) {
      case browser_input::KeyAction::MoveLeft:
        moved = moveSelection(browser, layout, -1, 0);
        break;
      case browser_input::KeyAction::MoveRight:
        moved = moveSelection(browser, layout, 1, 0);
        break;
      case browser_input::KeyAction::MoveUp:
        moved = moveSelection(browser, layout, 0, -1);
        break;
      case browser_input::KeyAction::MoveDown:
        moved = moveSelection(browser, layout, 0, 1);
        break;
      case browser_input::KeyAction::PageUp:
        moved = pageSelection(browser, layout, -1);
        break;
      case browser_input::KeyAction::PageDown:
        moved = pageSelection(browser, layout, 1);
        break;
      default:
        return changed;
    }
    changed = moved || changed;
    if (!moved) break;
  }
  return changed;
}

}  // namespace browser_selection_navigation
