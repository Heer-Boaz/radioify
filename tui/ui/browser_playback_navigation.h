#pragma once

#include <filesystem>
#include <functional>
#include <vector>

#include "browser_model.h"
#include "playback_target.h"

class BrowserNavigator;

class BrowserPlaybackNavigator {
 public:
  struct Callbacks {
    std::function<void()> markDirty;
    std::function<void()> markLayoutDirty;
  };

  BrowserPlaybackNavigator(BrowserNavigator& browserNavigator,
                           Callbacks callbacks);

  std::vector<PlaybackTarget> snapshotPlaybackTargets() const;
  bool activateTrackBrowser(const std::filesystem::path& file);
  bool revealPlaybackTarget(const PlaybackTarget& target);

 private:
  bool selectPlaybackTarget(const PlaybackTarget& target);

  BrowserNavigator& browserNavigator_;
  BrowserState& browser_;
  Callbacks callbacks_;
};
