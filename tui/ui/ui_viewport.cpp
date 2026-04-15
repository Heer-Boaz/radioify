#include "ui_viewport.h"

#include <algorithm>

BrowserViewport computeBrowserViewport(int screenWidth,
                                       int screenHeight,
                                       bool browserInteractionEnabled,
                                       bool showHeaderLabel,
                                       int footerLines,
                                       int searchBarClearButtonWidth) {
  BrowserViewport viewport;
  viewport.width = std::max(40, screenWidth);
  viewport.height = std::max(10, screenHeight);
  viewport.browserInteractionEnabled = browserInteractionEnabled;
  if (browserInteractionEnabled) {
    viewport.headerLines = 2 + (showHeaderLabel ? 1 : 0);
    viewport.searchBarY = 1;
    viewport.searchBarWidth = viewport.width;
    viewport.listTop = viewport.headerLines + 1;
    viewport.breadcrumbY = viewport.headerLines;
    viewport.searchBarClearEnd = viewport.width;
    viewport.searchBarClearStart =
        std::max(0, viewport.searchBarClearEnd - searchBarClearButtonWidth);
  } else {
    viewport.headerLines = 1;
    viewport.listTop = viewport.headerLines;
  }
  viewport.listHeight = viewport.height - viewport.listTop - footerLines;
  if (viewport.listHeight < 1) viewport.listHeight = 1;
  return viewport;
}
