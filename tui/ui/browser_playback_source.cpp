#include "browser_playback_source.h"

#include <utility>

#include "audio/media_formats.h"

namespace browser_playback_source {

playback_queue::Source capture(const std::vector<BrowserEntry>& entries) {
  std::vector<PlaybackTarget> targets;
  targets.reserve(entries.size());
  for (const BrowserEntry& entry : entries) {
    if (const auto* track = entry.actionAs<browser_entry::PlayTrack>()) {
      targets.push_back({entry.path, track->trackIndex});
    } else if (entry.actionAs<browser_entry::OpenFile>() &&
               (isSupportedAudioExt(entry.path) ||
                isSupportedVideoExt(entry.path))) {
      targets.push_back({entry.path, -1});
    }
  }
  return playback_queue::sourceFromTargets(std::move(targets));
}

}  // namespace browser_playback_source
