#pragma once

struct BrowserFooterLayout {
  int reservedLines = 0;
  bool showMeta = false;
  bool showWarning = false;
  bool showAnalyzeStatus = false;
  bool showLoopSplitStatus = false;
  bool showNowPlaying = true;
  int nowPlayingLines = 0;
  bool showActionStrip = false;
  int actionStripLines = 0;
  bool showPeakMeter = false;
  bool showProgress = true;
};

bool operator==(const BrowserFooterLayout& a, const BrowserFooterLayout& b);
bool operator!=(const BrowserFooterLayout& a, const BrowserFooterLayout& b);

BrowserFooterLayout computeBrowserFooterLayout(bool browserInteractionEnabled,
                                               bool showWarning,
                                               bool showAnalyzeStatus,
                                               bool showLoopSplitStatus,
                                               bool enableTransportUi,
                                               bool showNowPlaying,
                                               bool showPeakMeter);
