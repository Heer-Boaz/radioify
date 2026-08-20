#include "browser_playback_source.h"

#include <utility>

namespace browser_playback_source {

playback_queue::Source capture(const std::vector<BrowserEntry>& entries) {
  std::vector<PlaybackTarget> targets;
  targets.reserve(entries.size());
  for (const BrowserEntry& entry : entries) {
    if (const auto* track = entry.actionAs<browser_entry::PlayTrack>()) {
      targets.push_back({entry.path, track->trackIndex});
    } else if (entry.actionAs<browser_entry::OpenFile>()) {
      targets.push_back({entry.path, -1});
    }
  }
  return playback_queue::fromTargets(std::move(targets));
}

}  // namespace browser_playback_source
