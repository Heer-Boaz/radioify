#include "playback_transport_navigation.h"

#include <utility>

#include "browser_navigation.h"
#include "playback_target_match.h"
#include "playback_target_resolver.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"
#include "ui_helpers.h"
#include "ui_inputlogic.h"

namespace playback_transport_navigation {

Navigator::Navigator(BrowserState& browser, Callbacks callbacks)
    : browser_(browser), callbacks_(std::move(callbacks)) {}

BrowserState::Location Navigator::captureLocation() const {
  return captureBrowserLocation(browser_, browserLocationKind(browser_));
}

bool Navigator::activateTrackBrowser(const std::filesystem::path& file) {
  const BrowserState::Location from = captureLocation();
  if (!loadTrackBrowserForFile(file)) {
    return false;
  }
  browser_.dir = trackBrowserFile();
  browser_.selected = 0;
  browser_.scrollRow = 0;
  browser_.filter.clear();
  if (callbacks_.dirty) {
    setBrowserSearchFocus(browser_, BrowserSearchFocus::None, *callbacks_.dirty);
  }
  if (callbacks_.refreshBrowser) {
    callbacks_.refreshBrowser("");
  }
  if (callbacks_.markLayoutDirty) {
    callbacks_.markLayoutDirty();
  }
  recordBrowserNavigation(browser_, from, captureLocation());
  return true;
}

std::optional<PlaybackTarget> Navigator::resolveEntryTarget(
    const FileEntry& entry) const {
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
        !isTrackBrowserActive(browser_) || browser_.dir != trackPath;
    if (requiresRefresh && !activateTrackBrowser(target.file)) {
      return false;
    }
    if (selectPlaybackTarget(target)) {
      return true;
    }
    browser_.filter.clear();
    if (callbacks_.dirty) {
      setBrowserSearchFocus(browser_, BrowserSearchFocus::None, *callbacks_.dirty);
    }
    if (callbacks_.refreshBrowser) {
      callbacks_.refreshBrowser("");
    }
    if (callbacks_.markLayoutDirty) {
      callbacks_.markLayoutDirty();
    }
    return selectPlaybackTarget(target);
  }

  std::filesystem::path targetDir =
      target.file.has_parent_path() ? target.file.parent_path()
                                    : std::filesystem::path(".");
  const BrowserState::Location from = captureLocation();
  const bool requiresRefresh =
      isTrackBrowserActive(browser_) || browser_.dir != targetDir;
  if (requiresRefresh) {
    browser_.dir = targetDir;
    browser_.selected = 0;
    browser_.scrollRow = 0;
    browser_.filter.clear();
    if (callbacks_.dirty) {
      setBrowserSearchFocus(browser_, BrowserSearchFocus::None, *callbacks_.dirty);
    }
    if (callbacks_.refreshBrowser) {
      callbacks_.refreshBrowser(toUtf8String(target.file.filename()));
    }
    if (callbacks_.markLayoutDirty) {
      callbacks_.markLayoutDirty();
    }
  }
  if (selectPlaybackTarget(target)) {
    recordBrowserNavigation(browser_, from, captureLocation());
    return true;
  }
  browser_.filter.clear();
  if (callbacks_.dirty) {
    setBrowserSearchFocus(browser_, BrowserSearchFocus::None, *callbacks_.dirty);
  }
  if (callbacks_.refreshBrowser) {
    callbacks_.refreshBrowser(toUtf8String(target.file.filename()));
  }
  if (callbacks_.markLayoutDirty) {
    callbacks_.markLayoutDirty();
  }
  const bool selected = selectPlaybackTarget(target);
  recordBrowserNavigation(browser_, from, captureLocation());
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
