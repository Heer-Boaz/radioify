#pragma once

struct BrowserViewport {
  int width = 1;
  int height = 1;
  int headerLines = 0;
  int listTop = 0;
  int headerLabelY = -1;
  int breadcrumbY = -1;
  int searchBarY = -1;
  int searchBarWidth = 0;
  int searchBarClearStart = -1;
  int searchBarClearEnd = -1;
  int listHeight = 1;
  bool browserInteractionEnabled = true;
};

BrowserViewport computeBrowserViewport(int screenWidth,
                                       int screenHeight,
                                       bool browserInteractionEnabled,
                                       bool showHeaderLabel,
                                       int footerLines,
                                       int searchBarClearButtonWidth);
