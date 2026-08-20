#pragma once

#include <vector>

#include "browser_model.h"
#include "playback/target.h"

bool browserEntryMatchesPlaybackTarget(const BrowserEntry& entry,
                                       const PlaybackTarget& target);

int findBrowserPlaybackTargetEntry(const std::vector<BrowserEntry>& entries,
                                   const PlaybackTarget& target);
