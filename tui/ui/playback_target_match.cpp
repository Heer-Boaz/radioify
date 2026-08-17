#include "playback_target_match.h"

namespace {

std::filesystem::path comparablePlaybackPath(
    const std::filesystem::path& path) {
  return path.lexically_normal();
}

bool samePlaybackPath(const std::filesystem::path& left,
                      const std::filesystem::path& right) {
  if (left == right) {
    return true;
  }
  if (left.filename() != right.filename()) {
    return false;
  }
  return comparablePlaybackPath(left) == comparablePlaybackPath(right);
}

}  // namespace

bool browserEntryMatchesPlaybackTarget(const FileEntry& entry,
                                       const PlaybackTarget& target) {
  if (target.file.empty() || entry.isSectionHeader || entry.isDir) {
    return false;
  }
  if (!samePlaybackPath(entry.path, target.file)) {
    return false;
  }

  // A regular file entry represents its whole container, so it remains the
  // active item while one of that file's internal tracks is playing. Inside
  // the virtual track browser, only the exact track receives the marker.
  return entry.trackIndex < 0 || entry.trackIndex == target.trackIndex;
}

int findBrowserPlaybackTargetEntry(const std::vector<FileEntry>& entries,
                                   const PlaybackTarget& target) {
  for (size_t i = 0; i < entries.size(); ++i) {
    if (browserEntryMatchesPlaybackTarget(entries[i], target)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}
