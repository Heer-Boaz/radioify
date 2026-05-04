#include "playback_target_kind.h"

#include "media_formats.h"

PlaybackTargetKind classifyPlaybackTarget(const PlaybackTarget& target) {
  if (target.trackIndex >= 0) {
    return PlaybackTargetKind::Audio;
  }
  if (isSupportedImageExt(target.file)) {
    return PlaybackTargetKind::Image;
  }
  if (isSupportedVideoExt(target.file)) {
    return PlaybackTargetKind::Video;
  }
  return PlaybackTargetKind::Audio;
}
