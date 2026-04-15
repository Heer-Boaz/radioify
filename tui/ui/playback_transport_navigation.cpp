#include "playback_transport_navigation.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include "media_formats.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"
#include "ui_helpers.h"

namespace playback_transport_navigation {
namespace {

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool isVideoExt(const std::filesystem::path& path) {
  const std::string ext = toLower(toUtf8String(path.extension()));
  return ext == ".mp4" || ext == ".webm" || ext == ".mov" ||
         ext == ".mkv";
}

}  // namespace

Navigator::Navigator(BrowserState& browser, Callbacks callbacks)
    : browser_(browser), callbacks_(std::move(callbacks)) {}

bool Navigator::activateTrackBrowser(const std::filesystem::path& file) {
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
  return true;
}

std::optional<PlaybackTarget> Navigator::resolveEntryTarget(
    const FileEntry& entry) const {
  if (!isTransportPlayableEntry(entry)) {
    return std::nullopt;
  }
  PlaybackTarget target;
  target.file = entry.path;
  target.trackIndex = entry.trackIndex;
  if (target.trackIndex < 0 && isSupportedAudioExt(entry.path)) {
    std::vector<TrackEntry> tracks;
    std::string error;
    if (listTracksForFile(normalizeTrackBrowserPath(entry.path), &tracks,
                          &error) &&
        tracks.size() > 1) {
      target.file = normalizeTrackBrowserPath(entry.path);
      target.trackIndex = tracks.front().index;
    }
  }
  return target;
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
  return selectPlaybackTarget(target);
}

std::optional<PlaybackTarget> Navigator::resolveAdjacentPlaybackTarget(
    const PlaybackTarget& current, int direction) {
  if (direction == 0 || !syncBrowserToPlaybackTarget(current)) {
    return std::nullopt;
  }
  const int start = findPlaybackTargetEntryIndex(current);
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

bool Navigator::isTransportPlayableEntry(const FileEntry& entry) const {
  if (entry.isSectionHeader || entry.isDir) return false;
  if (entry.trackIndex >= 0) return true;
  return isSupportedAudioExt(entry.path) || isVideoExt(entry.path);
}

int Navigator::findPlaybackTargetEntryIndex(const PlaybackTarget& target) const {
  const std::filesystem::path trackPath =
      (target.trackIndex >= 0) ? normalizeTrackBrowserPath(target.file)
                               : std::filesystem::path();
  for (size_t i = 0; i < browser_.entries.size(); ++i) {
    const auto& entry = browser_.entries[i];
    if (target.trackIndex >= 0) {
      if (!isTransportPlayableEntry(entry)) {
        continue;
      }
      if (entry.trackIndex == target.trackIndex && entry.path == trackPath) {
        return static_cast<int>(i);
      }
      continue;
    }
    if (!entry.isSectionHeader && !entry.isDir && entry.trackIndex < 0 &&
        entry.path == target.file) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool Navigator::selectPlaybackTarget(const PlaybackTarget& target) {
  const int idx = findPlaybackTargetEntryIndex(target);
  if (idx < 0) {
    return false;
  }
  if (browser_.selected != idx) {
    browser_.selected = idx;
    if (callbacks_.markDirty) {
      callbacks_.markDirty();
    }
  }
  return true;
}

}  // namespace playback_transport_navigation
