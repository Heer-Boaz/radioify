#include "tui/ui/browser_action_strip.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << "browser_action_strip_tests: " << message << '\n';
  return false;
}

const browser_action_strip::Item* findItem(
    const std::vector<browser_action_strip::Item>& items,
    ActionStripItem id) {
  for (const browser_action_strip::Item& item : items) {
    if (item.id == id) {
      return &item;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  bool ok = true;
  browser_action_strip::Input input;
  input.playback.audioOk = true;
  input.playback.playPauseAvailable = true;
  input.playback.canPlayPrevious = true;
  input.playback.canPlayNext = true;
  input.playback.paused = true;
  input.playback.radioEnabled = true;
  input.playback.radioLabel = "Radio: Philco";
  input.playback.audioSupports50HzToggle = true;
  input.playback.hz50Enabled = true;
  input.playback.pictureInPictureAvailable = true;
  input.playback.pictureInPictureActive = true;
  input.browserControlsAvailable = true;
  input.viewMode = BrowserState::ViewMode::Thumbnails;
  input.optionsAvailable = true;
  input.optionsActive = true;

  const std::vector<browser_action_strip::Item> items =
      browser_action_strip::build(input);
  const browser_action_strip::Item* playPause =
      findItem(items, ActionStripItem::PlayPause);
  const browser_action_strip::Item* view =
      findItem(items, ActionStripItem::View);
  const browser_action_strip::Item* options =
      findItem(items, ActionStripItem::Options);
  ok &= expect(findItem(items, ActionStripItem::Previous) && playPause &&
                   findItem(items, ActionStripItem::Next) &&
                   findItem(items, ActionStripItem::Radio) &&
                   findItem(items, ActionStripItem::Hz50) &&
                   findItem(items, ActionStripItem::PictureInPicture),
               "playback controls must map into browser action identities");
  ok &= expect(playPause && playPause->active,
               "playback active state must survive the projection");
  ok &= expect(view && view->label.find("Grid") != std::string::npos,
               "the view action must describe the active browser mode");
  ok &= expect(options && options->active,
               "the options action must expose its active state");
  ok &= expect(view && view->width > 0,
               "action labels must include display-width metadata");

  browser_action_strip::Input playbackOnly;
  playbackOnly.playback.playPauseAvailable = true;
  const std::vector<browser_action_strip::Item> playbackItems =
      browser_action_strip::build(playbackOnly);
  ok &= expect(!findItem(playbackItems, ActionStripItem::View) &&
                   !findItem(playbackItems, ActionStripItem::Options),
               "browser-only actions must not leak into playback-only mode");

  ok &= expect(browser_action_strip::wrappedLineCount(items, 1) ==
                   static_cast<int>(items.size()) &&
                   browser_action_strip::wrappedLineCount(items, 1000) == 1,
               "action wrapping must be deterministic at narrow and wide widths");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
