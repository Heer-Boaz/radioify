#pragma once

#include <vector>

#include "browser_model.h"
#include "playback_target.h"

bool browserEntryMatchesPlaybackTarget(const FileEntry& entry,
                                       const PlaybackTarget& target);

int findBrowserPlaybackTargetEntry(const std::vector<FileEntry>& entries,
                                   const PlaybackTarget& target);
