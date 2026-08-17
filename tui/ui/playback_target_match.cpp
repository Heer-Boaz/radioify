#include "playback_target_match.h"

namespace {

PathIdentity entryPathIdentity(const FileEntry& entry) {
  return !entry.pathIdentity.empty() || entry.path.empty()
             ? entry.pathIdentity
             : makePathIdentity(entry.path);
}

bool entryMatchesTarget(const FileEntry& entry, const PlaybackTarget& target,
                        const PathIdentity& targetIdentity) {
  if (target.file.empty() || entry.path.empty() || entry.isSectionHeader ||
      entry.isDir || entryPathIdentity(entry) != targetIdentity) {
    return false;
  }

  // A regular file entry represents its whole container, so it remains the
  // active item while one of that file's internal tracks is playing. Inside
  // the virtual track browser, only the exact track receives the marker.
  return entry.trackIndex < 0 || entry.trackIndex == target.trackIndex;
}

}  // namespace

bool browserEntryMatchesPlaybackTarget(const FileEntry& entry,
                                       const PlaybackTarget& target) {
  return entryMatchesTarget(entry, target, makePathIdentity(target.file));
}

int findBrowserPlaybackTargetEntry(const std::vector<FileEntry>& entries,
                                   const PlaybackTarget& target) {
  if (target.file.empty()) {
    return -1;
  }
  const PathIdentity targetIdentity = makePathIdentity(target.file);
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entryMatchesTarget(entries[i], target, targetIdentity)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}
