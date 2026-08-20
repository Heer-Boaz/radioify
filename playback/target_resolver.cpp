#include "playback/target_resolver.h"

#include "audio/media_formats.h"
#include "playback/media/track_catalog.h"

namespace playback_target_resolver {
namespace {

std::optional<PlaybackTarget> resolveAudioPathTarget(
    const std::filesystem::path& path) {
  if (!isSupportedAudioExt(path)) {
    return std::nullopt;
  }

  const std::filesystem::path normalized = normalizePlaybackTrackPath(path);
  std::vector<TrackEntry> tracks;
  std::string error;
  if (listPlaybackTracks(normalized, &tracks, &error) && tracks.size() > 1) {
    return PlaybackTarget{normalized, tracks.front().index};
  }
  return PlaybackTarget{path, -1};
}

std::optional<PlaybackTarget> resolveMediaTarget(
    const std::filesystem::path& path) {
  if (isSupportedImageExt(path)) {
    return PlaybackTarget{path, -1};
  }
  return resolvePlaybackTarget(path);
}

}  // namespace

std::optional<PlaybackTarget> resolvePlaybackTarget(
    const std::filesystem::path& path) {
  if (path.empty()) {
    return std::nullopt;
  }
  if (isSupportedVideoExt(path)) {
    return PlaybackTarget{path, -1};
  }
  return resolveAudioPathTarget(path);
}

std::optional<PlaybackTarget> resolveDroppedTarget(
    const std::vector<std::filesystem::path>& files) {
  for (const std::filesystem::path& file : files) {
    if (std::optional<PlaybackTarget> target = resolveMediaTarget(file)) {
      return target;
    }
  }
  return std::nullopt;
}

}  // namespace playback_target_resolver
