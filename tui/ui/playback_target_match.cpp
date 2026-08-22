#include "playback_target_match.h"

namespace {

PathIdentity entryPathIdentity(const BrowserEntry& entry) {
  return !entry.pathIdentity.empty() || entry.path.empty()
             ? entry.pathIdentity
             : makePathIdentity(entry.path);
}

bool entryMatchesTarget(const BrowserEntry& entry, const PlaybackTarget& target,
                        const PathIdentity& targetIdentity) {
  if (playbackTargetFile(target).empty() || entry.path.empty() ||
      !entry.isMedia() ||
      entryPathIdentity(entry) != targetIdentity) {
    return false;
  }

  // A regular file entry represents its whole container, so it remains the
  // active item while one of that file's internal tracks is playing. Inside
  // the virtual track browser, only the exact track receives the marker.
  const auto* track = entry.actionAs<browser_entry::PlayTrack>();
  const std::optional<int> targetTrack = playbackTargetTrackIndex(target);
  return !track || (targetTrack && track->trackIndex == *targetTrack);
}

}  // namespace

bool browserEntryMatchesPlaybackTarget(const BrowserEntry& entry,
                                       const PlaybackTarget& target) {
  return entryMatchesTarget(entry, target,
                            makePathIdentity(playbackTargetFile(target)));
}

int findBrowserPlaybackTargetEntry(const std::vector<BrowserEntry>& entries,
                                   const PlaybackTarget& target) {
  if (playbackTargetFile(target).empty()) {
    return -1;
  }
  const PathIdentity targetIdentity =
      makePathIdentity(playbackTargetFile(target));
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entryMatchesTarget(entries[i], target, targetIdentity)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}
