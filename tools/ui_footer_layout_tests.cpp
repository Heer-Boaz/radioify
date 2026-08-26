#include "tui/ui/ui_footer_layout.h"

#include <cstdlib>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "ui_footer_layout_tests: " << message << '\n';
  return false;
}

}  // namespace

int main() {
  bool ok = true;

  const BrowserFooterLayout empty =
      computeBrowserFooterLayout(BrowserFooterLayoutInput{});
  ok &= expect(empty.reservedLines == 0,
               "an empty footer must not reserve any rows");

  BrowserFooterLayoutInput mediaTaskInput;
  mediaTaskInput.browserInteractionEnabled = true;
  mediaTaskInput.showMediaTaskStatus = true;
  const BrowserFooterLayout mediaTask =
      computeBrowserFooterLayout(mediaTaskInput);
  ok &= expect(mediaTask.showMeta && mediaTask.showMediaTaskStatus &&
                   mediaTask.reservedLines == 2,
               "a media task must reserve its own status row");

  BrowserFooterLayoutInput playbackInput;
  playbackInput.enableTransportUi = true;
  playbackInput.showNowPlaying = true;
  playbackInput.showPeakMeter = true;
  const BrowserFooterLayout playback =
      computeBrowserFooterLayout(playbackInput);
  ok &= expect(playback.showActionStrip && playback.showProgress &&
                   playback.showNowPlaying && playback.showPeakMeter &&
                   playback.reservedLines == 4,
               "playback controls must reserve each visible row");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
