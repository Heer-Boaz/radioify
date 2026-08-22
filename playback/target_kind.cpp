#include "playback/target_kind.h"

#include "audio/media_formats.h"

PlaybackTargetKind classifyPlaybackTarget(const PlaybackTarget& target) {
  if (playbackTargetIsTrack(target)) {
    return PlaybackTargetKind::Audio;
  }
  const std::filesystem::path& file = playbackTargetFile(target);
  if (isSupportedImageExt(file)) {
    return PlaybackTargetKind::Image;
  }
  if (isSupportedVideoExt(file)) {
    return PlaybackTargetKind::Video;
  }
  return PlaybackTargetKind::Audio;
}
