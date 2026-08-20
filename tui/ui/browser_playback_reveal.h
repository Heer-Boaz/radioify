#pragma once

#include <functional>

#include "browser_model.h"
#include "playback/target.h"

class BrowserNavigator;

class BrowserPlaybackRevealer {
 public:
  struct Callbacks {
    std::function<void()> markDirty;
    std::function<void()> markLayoutDirty;
  };

  BrowserPlaybackRevealer(BrowserNavigator& browserNavigator,
                          Callbacks callbacks);

  bool reveal(const PlaybackTarget& target);

 private:
  bool select(const PlaybackTarget& target);

  BrowserNavigator& browserNavigator_;
  BrowserState& browser_;
  Callbacks callbacks_;
};
