#include "browser_playback_navigation.h"

#include <utility>

#include "browser_navigation.h"
#include "playback_target_match.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"
#include "ui_helpers.h"

BrowserPlaybackNavigator::BrowserPlaybackNavigator(
    BrowserNavigator& browserNavigator, Callbacks callbacks)
    : browserNavigator_(browserNavigator), browser_(browserNavigator.state()),
      callbacks_(std::move(callbacks)) {}

std::vector<PlaybackTarget>
BrowserPlaybackNavigator::snapshotPlaybackTargets() const {
  std::vector<PlaybackTarget> targets;
  targets.reserve(browser_.entries.size());
  for (const BrowserEntry& entry : browser_.entries) {
    if (const auto* track = entry.actionAs<browser_entry::PlayTrack>()) {
      targets.push_back({entry.path, track->trackIndex});
    } else if (entry.actionAs<browser_entry::OpenFile>()) {
      // Container tracks are intentionally resolved only when transport reaches
      // this item. Capturing the sequence must remain a cheap immutable
      // snapshot of browser order, not a scan of every file in the directory.
      targets.push_back({entry.path, -1});
    }
  }
  return targets;
}

bool BrowserPlaybackNavigator::activateTrackBrowser(
    const std::filesystem::path& file) {
  return browserNavigator_.navigate(
      browserTrackLocation(normalizeTrackBrowserPath(file)));
}

bool BrowserPlaybackNavigator::revealPlaybackTarget(
    const PlaybackTarget& target) {
  if (target.file.empty()) {
    return false;
  }
  if (target.trackIndex >= 0) {
    const std::filesystem::path trackPath =
        normalizeTrackBrowserPath(target.file);
    const bool requiresRefresh =
        browser_.location != browserTrackLocation(trackPath);
    if (requiresRefresh && !activateTrackBrowser(target.file)) {
      return false;
    }
    if (selectPlaybackTarget(target)) {
      return true;
    }
    browser_.filter.clear();
    browser_.filterActive = false;
    browser_.pathSearch.clear();
    browser_.pathSearchActive = false;
    browserNavigator_.reload();
    return selectPlaybackTarget(target);
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
  if (selectPlaybackTarget(target)) {
    return true;
  }
  browser_.filter.clear();
  browser_.filterActive = false;
  browser_.pathSearch.clear();
  browser_.pathSearchActive = false;
  browserNavigator_.reload(toUtf8String(target.file.filename()));
  return selectPlaybackTarget(target);
}

bool BrowserPlaybackNavigator::selectPlaybackTarget(
    const PlaybackTarget& target) {
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
