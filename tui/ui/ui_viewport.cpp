#include "ui_viewport.h"

#include <algorithm>

BrowserViewport computeBrowserViewport(int screenWidth,
                                       int screenHeight,
                                       bool browserInteractionEnabled,
                                       bool showHeaderLabel,
                                       int footerLines,
                                       int searchBarClearButtonWidth) {
  BrowserViewport viewport;
  // Layout, rendering and hit-testing must share the physical viewport. A
  // fictional minimum size can place complete controls beyond the visible
  // terminal and turn an otherwise valid dialog into an invisible modal.
  viewport.width = std::max(1, screenWidth);
  viewport.height = std::max(1, screenHeight);
  viewport.browserInteractionEnabled = browserInteractionEnabled;
  if (browserInteractionEnabled) {
    if (viewport.height >= 2) {
      viewport.headerLines = 1;
      viewport.listTop = 1;
    }
    if (viewport.height >= 3) {
      viewport.searchBarY = 1;
      viewport.searchBarWidth = viewport.width;
      viewport.searchBarClearEnd = viewport.width;
      viewport.searchBarClearStart =
          std::max(0, viewport.searchBarClearEnd -
                          std::max(0, searchBarClearButtonWidth));
      viewport.headerLines = 2;
      viewport.listTop = 2;
    }
    if (viewport.height >= 4) {
      const bool headerLabelFits = showHeaderLabel && viewport.height >= 5;
      viewport.headerLabelY = headerLabelFits ? 2 : -1;
      viewport.headerLines = 2 + (headerLabelFits ? 1 : 0);
      viewport.breadcrumbY = viewport.headerLines;
      viewport.listTop = viewport.breadcrumbY + 1;
    }
  } else {
    viewport.headerLines = viewport.height >= 2 ? 1 : 0;
    viewport.listTop = viewport.headerLines;
  }
  viewport.listHeight = std::max(
      1, viewport.height - viewport.listTop - std::max(0, footerLines));
  return viewport;
}
