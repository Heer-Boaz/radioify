#include "tui/ui/browser_chrome.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "browser_chrome_tests: " << message << '\n';
  return false;
}

const browser_action_strip::Item* findItem(
    const browser_chrome::Model& model, ActionStripItem id) {
  for (const browser_action_strip::Item& item : model.actions) {
    if (item.id == id) return &item;
  }
  return nullptr;
}

}  // namespace

int main() {
  bool ok = true;

  browser_chrome::Input audioInput;
  audioInput.audio.source =
      AudioPlaybackSource{std::filesystem::path("song.flac"), std::nullopt};
  audioInput.audio.ready = true;
  audioInput.audio.paused = true;
  audioInput.audio.radioEnabled = true;
  audioInput.audio.radioFilterLabel = "Radio: Philco";
  audioInput.audio.supports50HzToggle = true;
  audioInput.audio.hz50Enabled = true;
  audioInput.playbackTargetAvailable = true;
  audioInput.audioPictureInPictureOpen = true;
  audioInput.transportUiEnabled = true;
  audioInput.optionsModeActive = true;
  audioInput.hasWarning = true;
  audioInput.hasMediaTaskStatus = true;
  audioInput.viewMode = BrowserState::ViewMode::Thumbnails;
  audioInput.nowPlayingLabel = "song.flac";
  audioInput.width = 200;

  const browser_chrome::Model audio = browser_chrome::build(audioInput);
  const browser_action_strip::Item* playPause =
      findItem(audio, ActionStripItem::PlayPause);
  const browser_action_strip::Item* pictureInPicture =
      findItem(audio, ActionStripItem::PictureInPicture);
  ok &= expect(audio.footer.showMeta && audio.footer.showWarning &&
                   audio.footer.showMediaTaskStatus &&
                   audio.footer.showNowPlaying &&
                   audio.footer.showActionStrip &&
                   audio.footer.showPeakMeter && audio.footer.showProgress,
               "audio playback must project all applicable footer regions");
  ok &= expect(playPause && playPause->active,
               "paused audio must present the Play action as active");
  ok &= expect(pictureInPicture && pictureInPicture->active,
               "an open audio picture-in-picture must be visible as active");
  ok &= expect(findItem(audio, ActionStripItem::Hz50) &&
                   findItem(audio, ActionStripItem::PitchMonitor) &&
                   findItem(audio, ActionStripItem::Options),
               "audio capabilities and browser state must project to actions");

  browser_chrome::Input unmeasuredInput = audioInput;
  unmeasuredInput.width = 0;
  const browser_chrome::Model unmeasured =
      browser_chrome::build(unmeasuredInput);
  ok &= expect(unmeasured.footer.actionStripLines == 1,
               "pre-layout chrome must retain its base action row");

  browser_chrome::Input videoInput;
  PlaybackControlState videoControl(
      playbackFileTarget(std::filesystem::path("movie.mp4")), true);
  videoControl.status = PlaybackControlStatus::Playing;
  videoInput.playback = videoControl;
  videoInput.playbackTargetAvailable = true;
  videoInput.videoPresentation =
      PlaybackPresentationState::nativeWindowed().togglePictureInPicture();
  videoInput.transportUiEnabled = true;
  videoInput.nowPlayingLabel = "movie.mp4";
  videoInput.width = 24;

  const browser_chrome::Model video = browser_chrome::build(videoInput);
  const browser_action_strip::Item* videoPictureInPicture =
      findItem(video, ActionStripItem::PictureInPicture);
  ok &= expect(videoPictureInPicture && videoPictureInPicture->active,
               "video PiP presentation state must drive the shared action");
  ok &= expect(!findItem(video, ActionStripItem::PitchMonitor),
               "pitch monitoring must not be offered for video playback");
  ok &= expect(video.footer.actionStripLines > 1 &&
                   video.footer.reservedLines > 3,
               "narrow chrome must reserve every wrapped action row");

  browser_chrome::Input melodyInput;
  melodyInput.melodyVisualizationActive = true;
  melodyInput.transportUiEnabled = false;
  melodyInput.optionsModeActive = true;
  melodyInput.width = 80;
  const browser_chrome::Model melody = browser_chrome::build(melodyInput);
  ok &= expect(!melody.footer.showMeta &&
                   !findItem(melody, ActionStripItem::View) &&
                   !findItem(melody, ActionStripItem::Options),
               "full-screen melody visualization must suppress browser chrome");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
