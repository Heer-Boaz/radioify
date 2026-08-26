#include "ui_footer_layout.h"

bool operator==(const BrowserFooterLayout& a, const BrowserFooterLayout& b) {
  return a.reservedLines == b.reservedLines && a.showMeta == b.showMeta &&
         a.showWarning == b.showWarning &&
         a.showMediaTaskStatus == b.showMediaTaskStatus &&
         a.showNowPlaying == b.showNowPlaying &&
         a.nowPlayingLines == b.nowPlayingLines &&
         a.showActionStrip == b.showActionStrip &&
         a.actionStripLines == b.actionStripLines &&
         a.showPeakMeter == b.showPeakMeter &&
         a.showProgress == b.showProgress;
}

bool operator!=(const BrowserFooterLayout& a, const BrowserFooterLayout& b) {
  return !(a == b);
}

BrowserFooterLayout computeBrowserFooterLayout(
    const BrowserFooterLayoutInput& input) {
  BrowserFooterLayout layout;
  layout.showMeta = input.browserInteractionEnabled;
  layout.showWarning = input.showWarning;
  layout.showMediaTaskStatus = input.showMediaTaskStatus;
  layout.showNowPlaying = input.showNowPlaying;
  layout.nowPlayingLines = layout.showNowPlaying ? 1 : 0;
  layout.showActionStrip = input.enableTransportUi;
  layout.actionStripLines = layout.showActionStrip ? 1 : 0;
  layout.showPeakMeter = input.showPeakMeter;
  layout.showProgress = input.enableTransportUi;

  layout.reservedLines = 0;
  layout.reservedLines += layout.showMeta ? 1 : 0;
  layout.reservedLines += layout.showWarning ? 1 : 0;
  layout.reservedLines += layout.showMediaTaskStatus ? 1 : 0;
  layout.reservedLines += layout.nowPlayingLines;
  layout.reservedLines += layout.actionStripLines;
  layout.reservedLines += layout.showPeakMeter ? 1 : 0;
  layout.reservedLines += layout.showProgress ? 1 : 0;
  return layout;
}
