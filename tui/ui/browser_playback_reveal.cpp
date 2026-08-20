#include "browser_playback_reveal.h"

#include <utility>

#include "browser_navigation.h"
#include "playback_target_match.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"
#include "ui_helpers.h"

BrowserPlaybackRevealer::BrowserPlaybackRevealer(
    BrowserNavigator& browserNavigator, Callbacks callbacks)
    : browserNavigator_(browserNavigator),
      browser_(browserNavigator.state()),
      callbacks_(std::move(callbacks)) {}

bool BrowserPlaybackRevealer::reveal(const PlaybackTarget& target) {
  if (target.file.empty()) {
    return false;
  }
  if (target.trackIndex >= 0) {
    const std::filesystem::path trackPath =
        normalizeTrackBrowserPath(target.file);
    const bool requiresRefresh =
        browser_.location != browserTrackLocation(trackPath);
    if (requiresRefresh &&
        !browserNavigator_.navigate(browserTrackLocation(trackPath))) {
      return false;
    }
    if (select(target)) {
      return true;
    }
    browser_.filter.clear();
    browser_.filterActive = false;
    browser_.pathSearch.clear();
    browser_.pathSearchActive = false;
    browserNavigator_.reload();
    return select(target);
  }

  const std::filesystem::path targetDir = target.file.has_parent_path()
                                              ? target.file.parent_path()
                                              : std::filesystem::path(".");
  const bool requiresRefresh =
      browser_.location != browserDirectoryLocation(targetDir);
  if (requiresRefresh &&
      !browserNavigator_.navigate(browserDirectoryLocation(targetDir),
                                  toUtf8String(target.file.filename()))) {
    return false;
  }
  if (select(target)) {
    return true;
  }
  browser_.filter.clear();
  browser_.filterActive = false;
  browser_.pathSearch.clear();
  browser_.pathSearchActive = false;
  browserNavigator_.reload(toUtf8String(target.file.filename()));
  return select(target);
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
