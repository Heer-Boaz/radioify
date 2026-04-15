#pragma once

struct BrowserViewport {
  int width = 40;
  int height = 10;
  int headerLines = 1;
  int listTop = 1;
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
