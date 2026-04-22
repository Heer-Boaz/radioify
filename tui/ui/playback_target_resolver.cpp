#include "playback_target_resolver.h"

#include "media_formats.h"
#include "track_browser_state.h"

namespace playback_target_resolver {
namespace {

std::optional<PlaybackTarget>
resolveAudioPathTarget(const std::filesystem::path& path) {
  if (!isSupportedAudioExt(path)) {
    return std::nullopt;
  }

  const std::filesystem::path normalized = normalizeTrackBrowserPath(path);
  std::vector<TrackEntry> tracks;
  std::string error;
  if (listTracksForFile(normalized, &tracks, &error) && tracks.size() > 1) {
    return PlaybackTarget{normalized, tracks.front().index};
  }
  return PlaybackTarget{path, -1};
}

}  // namespace

std::optional<PlaybackTarget>
resolvePathTarget(const std::filesystem::path& path,
                  PlaybackTargetResolveOptions options) {
  if (path.empty()) {
    return std::nullopt;
  }
  if (options.includeImages && isSupportedImageExt(path)) {
    return PlaybackTarget{path, -1};
  }
  if (isSupportedVideoExt(path)) {
    return PlaybackTarget{path, -1};
  }
  return resolveAudioPathTarget(path);
}

std::optional<PlaybackTarget>
resolveEntryTarget(const FileEntry& entry,
                   PlaybackTargetResolveOptions options) {
  if (entry.isSectionHeader || entry.isDir) {
    return std::nullopt;
  }
  if (entry.trackIndex >= 0) {
    return PlaybackTarget{entry.path, entry.trackIndex};
  }
  return resolvePathTarget(entry.path, options);
}

std::optional<PlaybackTarget>
resolveDroppedTarget(const std::vector<std::filesystem::path>& files) {
  PlaybackTargetResolveOptions options;
  options.includeImages = true;
  for (const auto& file : files) {
    if (auto target = resolvePathTarget(file, options)) {
      return target;
    }
  }
  return std::nullopt;
}

bool isPlayableEntry(const FileEntry& entry,
                     PlaybackTargetResolveOptions options) {
  return resolveEntryTarget(entry, options).has_value();
}

}  // namespace playback_target_resolver
