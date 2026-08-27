#pragma once

#include "browser_model.h"
#include "playback/target.h"

class BrowserNavigator;

class BrowserPlaybackRevealer {
 public:
  explicit BrowserPlaybackRevealer(BrowserNavigator& browserNavigator);

  bool reveal(const PlaybackTarget& target);

 private:
  BrowserNavigator& browserNavigator_;
};
