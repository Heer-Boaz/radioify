#pragma once

struct BrowserFooterLayoutInput {
  bool browserInteractionEnabled = false;
  bool showWarning = false;
  bool showMediaTaskStatus = false;
  bool enableTransportUi = false;
  bool showNowPlaying = false;
  bool showPeakMeter = false;
};

struct BrowserFooterLayout {
  int reservedLines = 0;
  bool showMeta = false;
  bool showWarning = false;
  bool showMediaTaskStatus = false;
  bool showNowPlaying = true;
  int nowPlayingLines = 0;
  bool showActionStrip = false;
  int actionStripLines = 0;
  bool showPeakMeter = false;
  bool showProgress = true;
};

bool operator==(const BrowserFooterLayout& a, const BrowserFooterLayout& b);
bool operator!=(const BrowserFooterLayout& a, const BrowserFooterLayout& b);

BrowserFooterLayout computeBrowserFooterLayout(
    const BrowserFooterLayoutInput& input);
