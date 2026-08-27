#include "tui/ui/browser_chrome.h"

#include <algorithm>

#include "core/unicode_display_width.h"
#include "playback/overlay/overlay.h"

namespace browser_chrome {
namespace {

int wrappedLabelLineCount(const std::string& label, int width) {
  if (width <= 0) return 1;
  const int displayWidth = utf8DisplayWidth(" " + label);
  return std::max(1, (displayWidth + width - 1) / width);
}

playback_overlay::PlaybackOverlayState playbackStateFor(
    const Input& input) {
  const bool videoActive = input.playback && input.playback->isVideo;
  const bool playbackAvailable = videoActive || input.audio.ready;

  playback_overlay::PlaybackOverlayState state;
  state.audioOk = playbackAvailable;
  state.playPauseAvailable = playbackAvailable;
  state.audioSupports50HzToggle =
      input.audio.ready && input.audio.supports50HzToggle;
  state.canPlayPrevious =
      input.playback ? input.playback->canPrevious
                     : playbackAvailable || input.playbackTargetAvailable;
  state.canPlayNext =
      input.playback ? input.playback->canNext
                     : playbackAvailable || input.playbackTargetAvailable;
  state.radioEnabled = input.audio.radioEnabled;
  state.radioLabel = input.audio.radioFilterLabel;
  state.hz50Enabled = input.audio.hz50Enabled;
  state.paused =
      input.playback
          ? input.playback->status != PlaybackControlStatus::Playing
          : input.audio.paused || input.audio.finished;
  state.pictureInPictureAvailable =
      videoActive || input.audioPictureInPictureOpen || playbackAvailable ||
      input.playbackTargetAvailable;
  state.pictureInPictureActive =
      input.videoPresentation
          ? input.videoPresentation->layer() ==
                PlaybackPresentationLayer::PictureInPicture
          : input.audioPictureInPictureOpen;
  return state;
}

}  // namespace

Model build(const Input& input) {
  const bool videoActive = input.playback && input.playback->isVideo;
  const bool audioTargetAvailable = input.audio.source.has_value();
  const bool browserInteractionEnabled =
      !input.melodyVisualizationActive;

  browser_action_strip::Input actionInput;
  actionInput.playback = playbackStateFor(input);
  actionInput.pitchMonitorAvailable =
      !videoActive && (input.melodyVisualizationActive || input.audio.ready ||
                       audioTargetAvailable);
  actionInput.pitchMonitorActive = input.melodyVisualizationActive;
  actionInput.browserControlsAvailable = browserInteractionEnabled;
  actionInput.viewMode = input.viewMode;
  actionInput.optionsAvailable =
      input.optionsModeActive || input.selectedEntryHasOptions;
  actionInput.optionsActive = input.optionsModeActive;

  Model model;
  model.actions = browser_action_strip::build(actionInput);
  model.nowPlayingLabel = input.nowPlayingLabel;

  BrowserFooterLayoutInput footerInput;
  footerInput.browserInteractionEnabled = browserInteractionEnabled;
  footerInput.showWarning = input.hasWarning;
  footerInput.showMediaTaskStatus = input.hasMediaTaskStatus;
  footerInput.enableTransportUi = input.transportUiEnabled;
  footerInput.showNowPlaying =
      input.playbackTargetAvailable || input.audio.ready ||
      input.audio.seeking || input.audio.holding;
  footerInput.showPeakMeter =
      input.transportUiEnabled && input.audio.ready;
  model.footer = computeBrowserFooterLayout(footerInput);

  if (model.footer.showNowPlaying) {
    const int lineCount =
        wrappedLabelLineCount(input.nowPlayingLabel, input.width);
    model.footer.reservedLines +=
        lineCount - model.footer.nowPlayingLines;
    model.footer.nowPlayingLines = lineCount;
  }
  if (model.footer.showActionStrip) {
    const int lineCount = std::max(
        1, browser_action_strip::wrappedLineCount(model.actions, input.width));
    model.footer.reservedLines +=
        lineCount - model.footer.actionStripLines;
    model.footer.actionStripLines = lineCount;
  }
  return model;
}

}  // namespace browser_chrome
