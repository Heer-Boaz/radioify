#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "browser_model.h"
#include "playback_target.h"

class BrowserNavigator;

namespace playback_transport_navigation {

class Navigator {
 public:
  struct Callbacks {
    std::function<void()> markDirty;
    std::function<void()> markLayoutDirty;
  };

  Navigator(BrowserNavigator& browserNavigator, Callbacks callbacks);

  bool activateTrackBrowser(const std::filesystem::path& file);
  std::optional<PlaybackTarget> resolveEntryTarget(
      const BrowserEntry& entry) const;
  bool syncBrowserToPlaybackTarget(const PlaybackTarget& target);
  std::optional<PlaybackTarget> resolveAdjacentPlaybackTarget(
      const PlaybackTarget& current, int direction);

 private:
  bool selectPlaybackTarget(const PlaybackTarget& target);

  BrowserNavigator& browserNavigator_;
  BrowserState& browser_;
  Callbacks callbacks_;
};

}  // namespace playback_transport_navigation
