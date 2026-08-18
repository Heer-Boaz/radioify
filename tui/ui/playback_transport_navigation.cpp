#include "playback_transport_navigation.h"

#include <utility>

#include "browser_navigation.h"
#include "playback_target_match.h"
#include "playback_target_resolver.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"
#include "ui_helpers.h"

namespace playback_transport_navigation {

Navigator::Navigator(BrowserNavigator& browserNavigator, Callbacks callbacks)
    : browserNavigator_(browserNavigator),
      browser_(browserNavigator.state()),
      callbacks_(std::move(callbacks)) {}

bool Navigator::activateTrackBrowser(const std::filesystem::path& file) {
  return browserNavigator_.navigate(
      browserTrackLocation(normalizeTrackBrowserPath(file)));
}

std::optional<PlaybackTarget> Navigator::resolveEntryTarget(
    const BrowserEntry& entry) const {
  return playback_target_resolver::resolveEntryTarget(entry);
}

bool Navigator::syncBrowserToPlaybackTarget(const PlaybackTarget& target) {
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

  std::filesystem::path targetDir =
      target.file.has_parent_path() ? target.file.parent_path()
                                    : std::filesystem::path(".");
  const bool requiresRefresh =
      browser_.location != browserDirectoryLocation(targetDir);
  if (requiresRefresh) {
    if (!browserNavigator_.navigate(
            browserDirectoryLocation(targetDir),
            toUtf8String(target.file.filename()))) {
      return false;
    }
  }
  if (selectPlaybackTarget(target)) {
    return true;
  }
  browser_.filter.clear();
  browser_.filterActive = false;
  browser_.pathSearch.clear();
  browser_.pathSearchActive = false;
  browserNavigator_.reload(toUtf8String(target.file.filename()));
  const bool selected = selectPlaybackTarget(target);
  return selected;
}

std::optional<PlaybackTarget> Navigator::resolveAdjacentPlaybackTarget(
    const PlaybackTarget& current, int direction) {
  if (direction == 0 || !syncBrowserToPlaybackTarget(current)) {
    return std::nullopt;
  }
  const int start =
      findBrowserPlaybackTargetEntry(browser_.entries, current);
  if (start < 0) {
    return std::nullopt;
  }
  const int count = static_cast<int>(browser_.entries.size());
  for (int idx = start + direction; idx >= 0 && idx < count;
       idx += direction) {
    if (auto target =
            resolveEntryTarget(browser_.entries[static_cast<size_t>(idx)])) {
      return target;
    }
  }
  return std::nullopt;
}

bool Navigator::selectPlaybackTarget(const PlaybackTarget& target) {
  const int idx = findBrowserPlaybackTargetEntry(browser_.entries, target);
  if (idx < 0) {
    return false;
  }
  if (browser_.selected != idx) {
    browser_.selected = idx;
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

}  // namespace playback_transport_navigation
