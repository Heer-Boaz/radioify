#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "browser_model.h"
#include "playback_target.h"

namespace playback_transport_navigation {

class Navigator {
 public:
  struct Callbacks {
    bool* dirty = nullptr;
    std::function<void()> markDirty;
    std::function<void()> markLayoutDirty;
    std::function<void(const std::string&)> refreshBrowser;
  };

  Navigator(BrowserState& browser, Callbacks callbacks);

  bool activateTrackBrowser(const std::filesystem::path& file);
  std::optional<PlaybackTarget> resolveEntryTarget(const FileEntry& entry) const;
  bool syncBrowserToPlaybackTarget(const PlaybackTarget& target);
  std::optional<PlaybackTarget> resolveAdjacentPlaybackTarget(
      const PlaybackTarget& current, int direction);

 private:
  bool isTransportPlayableEntry(const FileEntry& entry) const;
  int findPlaybackTargetEntryIndex(const PlaybackTarget& target) const;
  bool selectPlaybackTarget(const PlaybackTarget& target);

  BrowserState& browser_;
  Callbacks callbacks_;
};

}  // namespace playback_transport_navigation
