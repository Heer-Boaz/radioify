#include "ui_footer_layout.h"

bool operator==(const BrowserFooterLayout& a, const BrowserFooterLayout& b) {
  return a.reservedLines == b.reservedLines && a.showMeta == b.showMeta &&
         a.showWarning == b.showWarning &&
         a.showAnalyzeStatus == b.showAnalyzeStatus &&
         a.showLoopSplitStatus == b.showLoopSplitStatus &&
         a.showNowPlaying == b.showNowPlaying &&
         a.showActionStrip == b.showActionStrip &&
         a.showPeakMeter == b.showPeakMeter &&
         a.showProgress == b.showProgress;
}

bool operator!=(const BrowserFooterLayout& a, const BrowserFooterLayout& b) {
  return !(a == b);
}

BrowserFooterLayout computeBrowserFooterLayout(bool browserInteractionEnabled,
                                               bool showWarning,
                                               bool showAnalyzeStatus,
                                               bool showLoopSplitStatus,
                                               bool enableTransportUi,
                                               bool showNowPlaying,
                                               bool showPeakMeter) {
  BrowserFooterLayout layout;
  layout.showMeta = browserInteractionEnabled;
  layout.showWarning = showWarning;
  layout.showAnalyzeStatus = showAnalyzeStatus;
  layout.showLoopSplitStatus = showLoopSplitStatus;
  layout.showNowPlaying = showNowPlaying;
  layout.showActionStrip = enableTransportUi;
  layout.showPeakMeter = showPeakMeter;
  layout.showProgress = enableTransportUi;

  layout.reservedLines = 0;
  layout.reservedLines += layout.showMeta ? 1 : 0;
  layout.reservedLines += layout.showWarning ? 1 : 0;
  layout.reservedLines += layout.showAnalyzeStatus ? 1 : 0;
  layout.reservedLines += layout.showLoopSplitStatus ? 1 : 0;
  layout.reservedLines += layout.showNowPlaying ? 1 : 0;
  layout.reservedLines += layout.showActionStrip ? 1 : 0;
  layout.reservedLines += layout.showPeakMeter ? 1 : 0;
  layout.reservedLines += layout.showProgress ? 1 : 0;
  return layout;
}
