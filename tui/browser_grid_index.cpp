#include "browser_grid_index.h"

#include <algorithm>

int browserGridEntryIndex(const GridLayout& layout,
                          BrowserState::ViewMode mode, int row, int col,
                          int count) {
  if (row < 0 || col < 0 || col >= layout.cols) return -1;
  int idx = -1;
  if (mode == BrowserState::ViewMode::ListOnly) {
    idx = col * std::max(1, layout.rowsVisible) + row;
  } else {
    idx = row * layout.cols + col;
  }
  return (idx >= 0 && idx < count) ? idx : -1;
}

int browserGridVisibleCapacity(const GridLayout& layout) {
  return std::max(0, layout.rowsVisible) * std::max(1, layout.cols);
}

int browserGridTotalRowsForEntryCount(BrowserState::ViewMode mode, int count,
                                      int rowsVisible, int cols) {
  if (count <= 0) return 0;
  rowsVisible = std::max(1, rowsVisible);
  cols = std::max(1, cols);
  if (mode == BrowserState::ViewMode::ListOnly) {
    const int visibleCapacity = rowsVisible * cols;
    const int maxScroll = std::max(0, count - visibleCapacity);
    return rowsVisible + maxScroll;
  }
  return (count + cols - 1) / cols;
}
