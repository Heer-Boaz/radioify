#include "browser_playback_reveal.h"

#include "browser_navigation.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"

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
    BrowserNavigator& browserNavigator)
    : browserNavigator_(browserNavigator) {}

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
        browserNavigator_.state().location != browserTrackLocation(trackPath);
    if (requiresRefresh) {
      return browserNavigator_.reveal(browserTrackLocation(trackPath), {},
                                      playbackSelectionIdentity(target));
    }
    if (browserNavigator_.select(playbackSelectionIdentity(target))) {
      return true;
    }
    return browserNavigator_.reveal(browserNavigator_.state().location, {},
                                    playbackSelectionIdentity(target));
  }

  const std::filesystem::path targetDir = file.has_parent_path()
                                              ? file.parent_path()
                                              : std::filesystem::path(".");
  const bool requiresRefresh =
      browserNavigator_.state().location != browserDirectoryLocation(targetDir);
  if (requiresRefresh) {
    return browserNavigator_.reveal(
        browserDirectoryLocation(targetDir),
        toUtf8String(file.filename()),
        playbackSelectionIdentity(target));
  }
  if (browserNavigator_.select(playbackSelectionIdentity(target))) {
    return true;
  }
  return browserNavigator_.reveal(
      browserNavigator_.state().location, toUtf8String(file.filename()),
      playbackSelectionIdentity(target));
}
