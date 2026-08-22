#include "browser_playback_reveal.h"

#include <utility>

#include "browser_navigation.h"
#include "playback_target_match.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"
#include "ui_helpers.h"

namespace {

BrowserState::EntryIdentity playbackSelectionIdentity(
    const PlaybackTarget& target) {
  const std::filesystem::path& file = playbackTargetFile(target);
  const std::optional<int> trackIndex = playbackTargetTrackIndex(target);
  const std::filesystem::path path =
      trackIndex ? normalizeTrackBrowserPath(file) : file;
  BrowserEntry entry{
      {}, path,
      trackIndex
          ? browser_entry::Action(browser_entry::PlayTrack{*trackIndex})
          : browser_entry::Action(browser_entry::OpenFile{})};
  return browserEntryIdentity(entry);
}

}  // namespace

BrowserPlaybackRevealer::BrowserPlaybackRevealer(
    BrowserNavigator& browserNavigator, Callbacks callbacks)
    : browserNavigator_(browserNavigator),
      browser_(browserNavigator.state()),
      callbacks_(std::move(callbacks)) {}

bool BrowserPlaybackRevealer::reveal(const PlaybackTarget& target) {
  const std::filesystem::path& file = playbackTargetFile(target);
  const std::optional<int> trackIndex = playbackTargetTrackIndex(target);
  if (file.empty()) {
    return false;
  }
  if (trackIndex) {
    const std::filesystem::path trackPath =
        normalizeTrackBrowserPath(file);
    const bool requiresRefresh =
        browser_.location != browserTrackLocation(trackPath);
    if (requiresRefresh) {
      return browserNavigator_.reveal(browserTrackLocation(trackPath), {},
                                      playbackSelectionIdentity(target));
    }
    if (select(target)) {
      return true;
    }
    return browserNavigator_.reveal(browser_.location, {},
                                    playbackSelectionIdentity(target));
  }

  const std::filesystem::path targetDir = file.has_parent_path()
                                              ? file.parent_path()
                                              : std::filesystem::path(".");
  const bool requiresRefresh =
      browser_.location != browserDirectoryLocation(targetDir);
  if (requiresRefresh) {
    return browserNavigator_.reveal(
        browserDirectoryLocation(targetDir),
        toUtf8String(file.filename()),
        playbackSelectionIdentity(target));
  }
  if (select(target)) {
    return true;
  }
  return browserNavigator_.reveal(
      browser_.location, toUtf8String(file.filename()),
      playbackSelectionIdentity(target));
}

bool BrowserPlaybackRevealer::select(const PlaybackTarget& target) {
  const int index = findBrowserPlaybackTargetEntry(browser_.entries, target);
  if (index < 0) {
    return false;
  }
  if (browser_.selected != index) {
    browser_.selected = index;
    if (callbacks_.markDirty) {
      callbacks_.markDirty();
    }
  }
  requestBrowserSelectionReveal(browser_);
  if (callbacks_.markLayoutDirty) {
    callbacks_.markLayoutDirty();
  }
  return true;
}
